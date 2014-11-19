/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/SPDYCodec.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <folly/Conv.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <glog/logging.h>
#include <iostream>
#include <proxygen/lib/http/HTTPHeaderSize.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/codec/CodecDictionaries.h>
#include <proxygen/lib/http/codec/SPDYUtil.h>
#include <proxygen/lib/http/codec/compress/GzipHeaderCodec.h>
#include <proxygen/lib/http/codec/compress/HPACKCodec.h>
#include <proxygen/lib/utils/ParseURL.h>
#include <vector>

using folly::IOBuf;
using folly::IOBufQueue;
using folly::io::Appender;
using folly::io::Cursor;
using folly::io::RWPrivateCursor;
using proxygen::compress::Header;
using proxygen::compress::HeaderPieceList;
using std::pair;
using std::string;
using std::unique_ptr;
using std::vector;

namespace proxygen {

namespace {

// Sizes, in bytes, of various types and parts of SPDY frames
const size_t kFrameSizeDataCommon = 8;     // common prefix of all data frames
const size_t kFrameSizeControlCommon = 8;  // common prefix of all ctrl frames
const size_t kFrameSizeSynStream = 10;     // SYN_STREAM
const size_t kFrameSizeSynReplyv2 = 6;     // SYN_REPLY, SPDYv2
const size_t kFrameSizeSynReplyv3 = 4;     // SPDYv3's SYN_REPLY is shorter
const size_t kFrameSizeRstStream = 8;      // RST_STREAM
const size_t kFrameSizeGoawayv2 = 4;       // GOAWAY, SPDYv2
const size_t kFrameSizeGoawayv3 = 8;       // GOAWAY, SPDYv3
const size_t kFrameSizeHeaders = 4;        // HEADERS
const size_t kFrameSizePing = 4;           // PING
const size_t kFrameSizeSettings = 4;       // SETTINGS
const size_t kFrameSizeSettingsEntry = 8;  // Each entry in SETTINGS
const size_t kFrameSizeWindowUpdate = 8;   // WINDOW_UPDATE
const size_t kFrameSizeNameValuev2 = 2;    // The size in bytes of a
                                           // name/value pair
const size_t kFrameSizeNameValuev3 = 4;    // The size in bytes of a
                                           // name/value pair
const size_t kPriShiftv2 = 6;              // How many bits to shift pri, v2
const size_t kPriShiftv3 = 5;              // How many bits to shift pri, v3

#define CTRL_MASK 0x80
#define FLAGS_MASK 0xff000000
#define STREAM_ID_MASK 0x7fffffff
#define VERSION_MASK 0x7fff
#define DELTA_WINDOW_SIZE_MASK 0x7fffffff

/* The number of bytes of the frame header. */
#define FRAME_HEADER_LEN 8

// SPDY flags
const uint8_t kFlagFin = 0x01;

/**
 * Convenience function to pack SPDY's 8-bit flags field and
 * 24-bit length field into a single uint32_t so we can write
 * them out more easily.  (This function packs the flags into
 * the high order 8 bits of a 32-bit int; because SPDY uses
 * network byte ordering for these fields, the flag thus ends
 * up in the right place - in front of the length - when we
 * serialize the returned uint32_t.)
 */
uint32_t flagsAndLength(uint8_t flags, uint32_t length) {
  length &= 0x00ffffff;
  length |= (int32_t(flags) << 24);
  return length;
}

void appendUint16(uint8_t*& dst, size_t value) {
  *(uint16_t*)dst = htons(uint16_t(value));
  dst += 2;
}

void appendUint32(uint8_t*& dst, size_t value) {
  *(uint32_t*)dst = htonl(uint32_t(value));
  dst += 4;
}

uint32_t parseUint16(Cursor* cursor) {
  auto chunk = cursor->peek();
  if (LIKELY(chunk.second >= sizeof(uint16_t))) {
    cursor->skip(sizeof(uint16_t));
    return ntohs(*(uint16_t*)chunk.first);
  }
  return cursor->readBE<uint16_t>();
}

uint32_t parseUint32(Cursor* cursor) {
  auto chunk = cursor->peek();
  if (LIKELY(chunk.second >= sizeof(uint32_t))) {
    cursor->skip(sizeof(uint32_t));
    return ntohl(*(uint32_t*)chunk.first);
  }
  return cursor->readBE<uint32_t>();
}

class SPDYSessionFailed : public std::exception {
 public:
  explicit SPDYSessionFailed(spdy::GoawayStatusCode inStatus)
      : statusCode(inStatus) {}

  spdy::GoawayStatusCode statusCode;
};

class SPDYStreamFailed : public std::exception {
 public:
  SPDYStreamFailed(bool inIsNew, uint32_t inStreamID,
                   uint32_t inStatus,
                   const std::string& inMsg = empty_string)
      : isNew(inIsNew),
        streamID(inStreamID),
        statusCode(inStatus) {
    message = folly::to<std::string>("new=", isNew, " streamID=", streamID,
                                     " statusCode=", statusCode, " message=",
                                     inMsg);
  }

  ~SPDYStreamFailed() throw() {}

  virtual const char* what() const throw() {
    return message.c_str();
  }

  bool isNew;
  uint32_t streamID;
  uint32_t statusCode;
  std::string message;
};

void printCtrlHeader(uint16_t version, uint8_t flags, uint32_t length) {
  std::cout << "CTRL FRAME: version=" << version << ", flags="
            <<  std::hex << folly::to<unsigned int>(flags) << std::dec
            << ", length=" << length << std::endl;
}

void printNV(const compress::HeaderPieceList& headers) {
  for (size_t i = 0; i < headers.size(); i += 2) {
    std::cout << "\t" << headers[i].str << ": "
              << headers[i + 1].str << std::endl;
  }
}

void printHeaders(uint32_t stream_id,
                  const compress::HeaderPieceList& headers) {
  std::cout << "HEADERS: stream_id=" << stream_id
            << "numHeaders=" << headers.size() / 2 << std::endl;
  printNV(headers);
}

void printSynStream(uint32_t stream_id, uint32_t assocStream, uint8_t pri,
                    uint8_t slot,
                    const compress::HeaderPieceList& headers) {
  std::cout << "SYN_STREAM: stream_id=" << stream_id << ", assocStream="
            << assocStream << ", pri=" << folly::to<unsigned int>(pri)
            << ", slot=" << folly::to<unsigned int>(slot)
            << ", numHeaders=" << (headers.size() / 2) << std::endl;
  printNV(headers);
}

void printSynReply(uint32_t stream_id,
                   const compress::HeaderPieceList& headers) {
  std::cout << "SYN_REPLY: stream_id=" << stream_id
            << ", numHeaders=" << headers.size() / 2 << std::endl;

  printNV(headers);
}

void printRstStream(uint32_t stream_id, uint32_t statusCode) {
  std::cout << "RST_STREAM: stream_id=" << stream_id << ", statusCode="
            << statusCode << std::endl;
}

void printSettings(const SPDYCodec::SettingList& settings) {
  std::cout << "SETTINGS: num=" << settings.size() << std::endl;
  for (const auto& setting: settings) {
    std::cout << "\tflags="
              << std::hex << folly::to<unsigned int>(setting.flags) << std::dec
              << ", id=" << setting.id
              << ", value=" << setting.value << std::endl;
  }
}

void printPing(uint32_t unique_id) {
  std::cout << "PING: unique_id=" << unique_id << std::endl;
}

void printGoaway(uint32_t lastGoodStream, uint32_t statusCode) {
  std::cout << "GOAWAY: lastGoodStream=" << lastGoodStream
            << ", statusCode=" << statusCode << std::endl;
}

void printWindowUpdate(uint32_t stream_id, uint32_t delta) {
  std::cout << "WINDOW_UPDATE: stream_id=" << stream_id
            << "delta_window_size=" << delta << std::endl;
}

void printDataFrame(uint32_t stream_id, uint8_t flags, uint32_t length) {
  std::cout << "DATA: stream_id=" << stream_id << ", flags="
            << std::hex << folly::to<unsigned int>(flags) << std::dec
            << ", length=" << length << std::endl;
}

void printException(const std::exception& ex) {
  std::cout << "Exception: " << folly::exceptionStr(ex) << std::endl;
}

} // anonynous namespace

std::bitset<256> SPDYCodec::perHopHeaderCodes_;

void SPDYCodec::initPerHopHeaders() {
  // SPDY per-hop headers
  perHopHeaderCodes_[HTTP_HEADER_CONNECTION] = true;
  perHopHeaderCodes_[HTTP_HEADER_HOST] = true;
  perHopHeaderCodes_[HTTP_HEADER_KEEP_ALIVE] = true;
  perHopHeaderCodes_[HTTP_HEADER_PROXY_CONNECTION] = true;
  perHopHeaderCodes_[HTTP_HEADER_TRANSFER_ENCODING] = true;
  perHopHeaderCodes_[HTTP_HEADER_UPGRADE] = true;
}

const SPDYVersionSettings& SPDYCodec::getVersionSettings(SPDYVersion version) {
  // Indexed by SPDYVersion
  static const SPDYVersionSettings spdyVersions[] = {
  // SPDY2
    {spdy::kNameVersionv2, spdy::kNameStatusv2, spdy::kNameMethodv2,
    spdy::kNamePathv2, spdy::kNameSchemev2, "",
     spdy::kSessionProtoNameSPDY2, parseUint16, appendUint16,
    (const unsigned char*)kSPDYv2Dictionary, sizeof(kSPDYv2Dictionary),
    0x8002, kFrameSizeSynReplyv2, kFrameSizeNameValuev2,
     kFrameSizeGoawayv2, kPriShiftv2, 2, 0, SPDYVersion::SPDY2},
  // SPDY3
    {spdy::kNameVersionv3, spdy::kNameStatusv3, spdy::kNameMethodv3,
    spdy::kNamePathv3, spdy::kNameSchemev3, spdy::kNameHostv3,
    spdy::kSessionProtoNameSPDY3, parseUint32, appendUint32,
    (const unsigned char*)kSPDYv3Dictionary, sizeof(kSPDYv3Dictionary),
    0x8003, kFrameSizeSynReplyv3, kFrameSizeNameValuev3,
     kFrameSizeGoawayv3, kPriShiftv3, 3, 0, SPDYVersion::SPDY3},
  // SPDY3.1
    {spdy::kNameVersionv3, spdy::kNameStatusv3, spdy::kNameMethodv3,
    spdy::kNamePathv3, spdy::kNameSchemev3, spdy::kNameHostv3,
    spdy::kSessionProtoNameSPDY3, parseUint32, appendUint32,
    (const unsigned char*)kSPDYv3Dictionary, sizeof(kSPDYv3Dictionary),
    0x8003, kFrameSizeSynReplyv3, kFrameSizeNameValuev3,
     kFrameSizeGoawayv3, kPriShiftv3, 3, 1, SPDYVersion::SPDY3_1}
  };
  // SPDY3_1_HPACK is identical to SPDY3 in terms of version settings structure
  if (version == SPDYVersion::SPDY3_1_HPACK) {
    version = SPDYVersion::SPDY3_1;
  }
  auto intVersion = static_cast<unsigned>(version);
  CHECK(intVersion < (sizeof(spdyVersions) / sizeof(SPDYVersionSettings)));
  return spdyVersions[intVersion];
}

SPDYCodec::SPDYCodec(TransportDirection direction, SPDYVersion version,
                     int spdyCompressionLevel /* = Z_NO_COMPRESSION */)
  : callback_(nullptr),
    transportDirection_(direction),
    lastStreamID_(0),
    versionSettings_(getVersionSettings(version)),
    maxFrameLength_(spdy::kMaxFrameLength),
    currentIngressBuf_(nullptr),
    frameState_(FrameState::FRAME_HEADER),
    version_(0),
    type_(0xffff),
    streamId_(0),
    length_(0),
    flags_(0),
    sessionClosing_(ClosingState::OPEN),
    printer_(false),
    ctrl_(false) {
  VLOG(4) << "creating SPDY/" << static_cast<int>(versionSettings_.majorVersion)
          << "." << static_cast<int>(versionSettings_.minorVersion)
          << " codec";
  if (version == SPDYVersion::SPDY3_1_HPACK) {
    headerCodec_ = folly::make_unique<HPACKCodec>(transportDirection_);
  } else {
    headerCodec_ = folly::make_unique<GzipHeaderCodec>(
      spdyCompressionLevel, versionSettings_);
    // Use the default value.
    headerCodec_->setMaxUncompressed(proxygen::spdy::kMaxFrameLength);
  }

  switch (transportDirection_) {
  case TransportDirection::DOWNSTREAM:
    nextEgressStreamID_ = 2;
    nextEgressPingID_ = 2;
    break;
  case TransportDirection::UPSTREAM:
    nextEgressStreamID_ = 1;
    nextEgressPingID_ = 1;
    break;
  default:
    LOG(FATAL) << "Unknown transport direction.";
  }
}

SPDYCodec::~SPDYCodec() {
}

void SPDYCodec::setMaxFrameLength(uint32_t maxFrameLength) {
  maxFrameLength_ = maxFrameLength;
}

void SPDYCodec::setMaxUncompressedHeaders(uint32_t maxUncompressed) {
  headerCodec_->setMaxUncompressed(maxUncompressed);
}

CodecProtocol SPDYCodec::getProtocol() const {
  switch (versionSettings_.version) {
    case SPDYVersion::SPDY2: return CodecProtocol::SPDY_2;
    case SPDYVersion::SPDY3: return CodecProtocol::SPDY_3;
    case SPDYVersion::SPDY3_1: return CodecProtocol::SPDY_3_1;
    case SPDYVersion::SPDY3_1_HPACK: return CodecProtocol::SPDY_3_1_HPACK;
  };
  LOG(FATAL) << "unreachable";
  return CodecProtocol::SPDY_3_1;
}

bool SPDYCodec::supportsStreamFlowControl() const {
  return versionSettings_.majorVersion > 2;
}

bool SPDYCodec::supportsSessionFlowControl() const {
  return versionSettings_.majorVersion > 3 ||
    (versionSettings_.majorVersion == 3 && versionSettings_.minorVersion > 0);
}

HTTPCodec::StreamID SPDYCodec::createStream() {
  auto ret = nextEgressStreamID_;
  nextEgressStreamID_ += 2;
  return ret;
}

bool SPDYCodec::isBusy() const {
  return false;
}

void SPDYCodec::setParserPaused(bool paused) {
  // Not applicable
}

void SPDYCodec::checkLength(uint32_t expectedLength, const std::string& msg) {
  if (length_ != expectedLength) {
    LOG(ERROR) << msg << ": invalid length " << length_ << " != " <<
      expectedLength;
    throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
  }
}

void SPDYCodec::checkMinLength(uint32_t minLength, const std::string& msg) {
  if (length_ < minLength) {
    LOG(ERROR) << msg << ": invalid length " << length_ << " < " << minLength;
    throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
  }
}

size_t SPDYCodec::onIngress(const folly::IOBuf& buf) {
  size_t bytesParsed = 0;
  currentIngressBuf_ = &buf;
  try {
    bytesParsed = parseIngress(buf);
  } catch (const SPDYSessionFailed& ex) {
    if (printer_) {
      printException(ex);
    }
    failSession(ex.statusCode);
    bytesParsed = buf.computeChainDataLength();
  }
  return bytesParsed;
}

size_t SPDYCodec::parseIngress(const folly::IOBuf& buf) {
  const size_t chainLength = buf.computeChainDataLength();
  Cursor cursor(&buf);
  size_t avail = cursor.totalLength();

  // This can parse beyond the current IOBuf
  for (; avail > 0; avail = cursor.totalLength()) {
    if (frameState_ == FrameState::FRAME_HEADER) {
      if (avail < FRAME_HEADER_LEN) {
        // Make the caller buffer until we get a full frame header
        break;
      }
      auto data = cursor.peek();
      ctrl_ = (data.first[0] & CTRL_MASK);
      if (ctrl_) {
        version_ = cursor.readBE<uint16_t>() & VERSION_MASK;
        type_  = cursor.readBE<uint16_t>();
        if (version_ != versionSettings_.majorVersion) {
          LOG(ERROR) << "Invalid version=" << version_;
          throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
        }
      } else {
        streamId_ = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
      }
      length_ = cursor.readBE<uint32_t>();
      flags_ = (length_ & FLAGS_MASK) >> 24;
      length_ &= ~FLAGS_MASK;
      if (ctrl_) {
        if (length_ > maxFrameLength_) {
          if (type_ == spdy::SYN_STREAM || type_ == spdy::SYN_REPLY ||
              type_ == spdy::HEADERS) {
            uint32_t stream_id = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
            failStream(true, stream_id, spdy::RST_FRAME_TOO_LARGE);
            // Compression/stream state is out of sync now
          }
          // Since maxFrameLength_ must be at least 8kb and most control frames
          // have fixed size, only an invalid settings or credential frame can
          // land here. For invalid credential frames we must send a goaway,
          // and a settings frame would have > 1023 pairs, of which none are
          // allowed to be duplicates. Just fail everything.
          LOG(ERROR) << "excessive frame size length_=" << length_;
          throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
        }
        frameState_ = FrameState::CTRL_FRAME_DATA;
        if (printer_) {
          printCtrlHeader(version_, flags_, length_);
        }
      } else {
        frameState_ = FrameState::DATA_FRAME_DATA;
        if (printer_) {
          printDataFrame(streamId_, flags_, length_);
        }
      }
    } else if (frameState_ == FrameState::CTRL_FRAME_DATA) {
      if (avail < length_) {
        // Make the caller buffer the rest of the control frame.
        // We could attempt to decompress incomplete name/value blocks,
        // but for now we're favoring simplicity.
        VLOG(6) << "Need more data: length_=" << length_ << " avail=" << avail;
        break;
      }
      try {
        onControlFrame(cursor);
      } catch (const SPDYStreamFailed& ex) {
        if (printer_) {
          printException(ex);
        }
        failStream(ex.isNew, ex.streamID, ex.statusCode, ex.what());
      }
      frameState_ = FrameState::FRAME_HEADER;
    } else if (avail > 0 || length_ == 0) {
      // Data frame data.  Pass everything we have up to the frame boundary
      DCHECK(FrameState::DATA_FRAME_DATA == frameState_);

      uint32_t toClone = (avail > std::numeric_limits<uint32_t>::max()) ?
        std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(avail);
      toClone = std::min(toClone, length_);
      std::unique_ptr<IOBuf> chunk;
      cursor.clone(chunk, toClone);
      callback_->onBody(StreamID(streamId_), std::move(chunk));
      length_ -= toClone;
    }

    // Fin handling
    if (length_ == 0) {
      if (flags_ & spdy::CTRL_FLAG_FIN) {
        callback_->onMessageComplete(StreamID(streamId_), false);
      }
      frameState_ = FrameState::FRAME_HEADER;
    }
  }
  return chainLength - avail;
}

void SPDYCodec::onControlFrame(Cursor& cursor) {
  uint32_t stream_id = 0;
  switch (type_) {
    case spdy::SYN_STREAM:
    {
      checkMinLength(kFrameSizeSynStream, "SYN_STREAM");
      streamId_ = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
      uint32_t assocStream = cursor.readBE<uint32_t>();
      uint8_t pri = cursor.read<uint8_t>() >> versionSettings_.priShift;
      uint8_t slot = cursor.read<uint8_t>();
      length_ -= kFrameSizeSynStream;
      auto result = decodeHeaders(cursor);
      checkLength(0, "SYN_STREAM");
      onSynStream(assocStream, pri, slot,
                  result.headers, headerCodec_->getDecodedSize());
      break;
    }
    case spdy::SYN_REPLY:
    {
      checkMinLength(versionSettings_.synReplySize, "SYN_REPLY");
      streamId_ = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
      length_ -= versionSettings_.synReplySize;
      if (version_ == 2) {
        // 2 byte unused
        cursor.skip(2);
      }
      auto result = decodeHeaders(cursor);
      checkLength(0, "SYN_REPLY");
      onSynReply(result.headers,
                 headerCodec_->getDecodedSize());
      break;
    }
    case spdy::RST_STREAM:
    {
      checkLength(kFrameSizeRstStream, "RST");
      streamId_ = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
      uint32_t statusCode = cursor.readBE<uint32_t>();
      onRstStream(statusCode);
      break;
    }
    case spdy::SETTINGS:
    {
      checkMinLength(kFrameSizeSettings, "SETTINGS");
      uint32_t numSettings = cursor.readBE<uint32_t>();
      length_ -= sizeof(uint32_t);
      if (length_ / 8 < numSettings) {
        LOG(ERROR) << "SETTINGS: number of settings to high. "
                   << length_ << " < 8 * " << numSettings;
        throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
      }
      SettingList settings;
      for (uint32_t i = 0; i < numSettings; i++) {
        uint32_t id = 0;
        if (version_ == 2) {
          id = cursor.readLE<uint32_t>();
        } else {
          id = cursor.readBE<uint32_t>();
        }
        uint32_t value = cursor.readBE<uint32_t>();
        uint8_t flags = (id & FLAGS_MASK) >> 24;
        id &= ~FLAGS_MASK;
        settings.emplace_back(flags, id, value);
      }
      onSettings(settings);
      break;
    }
    case spdy::NOOP:
      VLOG(4) << "Noop received. Doing nothing.";
      checkLength(0, "NOOP");
      break;
    case spdy::PING:
    {
      checkLength(kFrameSizePing, "PING");
      uint32_t unique_id = cursor.readBE<uint32_t>();
      onPing(unique_id);
      break;
    }
    case spdy::GOAWAY:
    {
      checkLength(versionSettings_.goawaySize, "GOAWAY");
      uint32_t lastStream = cursor.readBE<uint32_t>();
      uint32_t statusCode = 0;
      if (version_ == 3) {
        statusCode = cursor.readBE<uint32_t>();
      }
      onGoaway(lastStream, statusCode);
      break;
    }
    case spdy::HEADERS:
    {
      // Note: this is for the HEADERS frame type, not the initial headers
      checkMinLength(kFrameSizeHeaders, "HEADERS");
      streamId_ = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
      length_ -= kFrameSizeHeaders;
      if (version_ == 2) {
        // 2 byte unused
        cursor.skip(2);
        length_ -= 2;
      }
      auto result = decodeHeaders(cursor);
      checkLength(0, "HEADERS");
      onHeaders(result.headers);
      break;
    }
    case spdy::WINDOW_UPDATE:
    {
      checkLength(kFrameSizeWindowUpdate, "WINDOW_UPDATE");
      streamId_ = cursor.readBE<uint32_t>() & STREAM_ID_MASK;
      uint32_t delta = cursor.readBE<uint32_t>() & DELTA_WINDOW_SIZE_MASK;
      onWindowUpdate(delta);
      break;
    }
    case spdy::CREDENTIAL:
    {
      VLOG(4) << "Skipping unsupported/deprecated CREDENTIAL frame";
      // Fall through to default case
    }
    default:
      VLOG(3) << "unimplemented control frame type " << type_
              << ", frame length: " << length_;
      // From spdy spec:
      // Control frame processing requirements:
      // If an endpoint receives a control frame for a type it does not
      // recognize, it must ignore the frame.

      // Consume rest of the frame to skip processing it further
      cursor.skip(length_);
      length_ = 0;
      return;
  }
}

HeaderDecodeResult SPDYCodec::decodeHeaders(Cursor& cursor) {
  auto result = headerCodec_->decode(cursor, length_);
  if (result.isError()) {
    auto err = result.error();
    if (err == HeaderDecodeError::HEADERS_TOO_LARGE ||
        err == HeaderDecodeError::INFLATE_DICTIONARY ||
        err == HeaderDecodeError::BAD_ENCODING) {
      // Fail stream only for FRAME_TOO_LARGE error
      if (err == HeaderDecodeError::HEADERS_TOO_LARGE) {
        failStream(true, streamId_, spdy::RST_FRAME_TOO_LARGE);
      }
      throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
    }
    // For other types of errors we throw a stream error
    bool newStream = (type_ != spdy::HEADERS);
    throw SPDYStreamFailed(newStream, streamId_, spdy::RST_PROTOCOL_ERROR,
                           "Error parsing header: " + folly::to<string>(err));
  }

  length_ -= result.ok().bytesConsumed;
  return result.ok();
}

void SPDYCodec::onIngressEOF() {
  // SPDY does not report errors for partial frames
}

bool SPDYCodec::isReusable() const {
  // This codec can process new streams if it is open, or if it is a
  // server and it has only sent the first of two goaways.
  // TODO move ingressGoawayAck_ into ClosingState and simplify this logic.
  return (sessionClosing_ == ClosingState::OPEN ||
          sessionClosing_ == ClosingState::OPEN_WITH_GRACEFUL_DRAIN_ENABLED ||
          (transportDirection_ == TransportDirection::DOWNSTREAM &&
           isWaitingToDrain()))
    && (ingressGoawayAck_ == std::numeric_limits<uint32_t>::max());
}

bool SPDYCodec::isWaitingToDrain() const {
  return sessionClosing_ == ClosingState::FIRST_GOAWAY_SENT;
}

bool SPDYCodec::isSPDYReserved(const std::string& name) {
  return (versionSettings_.majorVersion == 2 &&
          ((transportDirection_ == TransportDirection::DOWNSTREAM &&
            (boost::iequals(name, spdy::kNameStatusv2) ||
             boost::iequals(name, spdy::kNameVersionv2))) ||
           (transportDirection_ == TransportDirection::UPSTREAM &&
            (boost::iequals(name, spdy::kNameMethodv2) ||
             boost::iequals(name, spdy::kNameSchemev2) ||
             boost::iequals(name, spdy::kNamePathv2) ||
             boost::iequals(name, spdy::kNameVersionv2)))));
}

// Add the SPDY-specific header fields that hold the
// equivalent of the HTTP/1.x request-line or status-line.
unique_ptr<IOBuf> SPDYCodec::encodeHeaders(
  const HTTPMessage& msg, vector<Header>& allHeaders,
  uint32_t headroom, HTTPHeaderSize* size) {

  allHeaders.emplace_back(versionSettings_.versionStr, spdy::httpVersion);

  // Add the HTTP headers supplied by the caller, but skip
  // any per-hop headers that aren't supported in SPDY.
  msg.getHeaders().forEachWithCode([&] (HTTPHeaderCode code,
                                        const string& name,
                                        const string& value) {
    if (perHopHeaderCodes_[code] || isSPDYReserved(name)) {
      VLOG(3) << "Dropping SPDY reserved header " << name;
      return;
    }
    if (name.length() == 0) {
      VLOG(2) << "Dropping header with empty name";
      return;
    }
    if (versionSettings_.majorVersion == 2 && value.length() == 0) {
      VLOG(2) << "Dropping header \"" << name
              << "\" with empty value for spdy/2";
      return;
    }
    allHeaders.emplace_back(code, name, value);
  });

  headerCodec_->setEncodeHeadroom(headroom);
  auto out = headerCodec_->encode(allHeaders);
  if (size) {
    *size = headerCodec_->getEncodedSize();
  }

  return out;
}

unique_ptr<IOBuf> SPDYCodec::serializeResponseHeaders(
  const HTTPMessage& msg, uint32_t headroom, HTTPHeaderSize* size) {

  // Note: the header-sorting code works with pointers to strings.
  // The role of this local status string is to hold the generated
  // status code long enough for the sort (done later within the
  // same scope) to be able to access it.
  string status;

  const HTTPHeaders& headers = msg.getHeaders();
  vector<Header> allHeaders;
  allHeaders.reserve(headers.size() + 4);

  if (msg.getStatusMessage().empty()) {
    status = folly::to<string>(msg.getStatusCode());
  } else {
    status = folly::to<string>(msg.getStatusCode(), " ",
                               msg.getStatusMessage());
  }
  allHeaders.emplace_back(versionSettings_.statusStr, status);
  // See comment above regarding status
  string date;
  if (!headers.exists(HTTP_HEADER_DATE)) {
    date = std::move(HTTPMessage::formatDateHeader());
    allHeaders.emplace_back(HTTP_HEADER_DATE, date);
  }

  return encodeHeaders(msg, allHeaders, headroom, size);
}

unique_ptr<IOBuf> SPDYCodec::serializeRequestHeaders(
  const HTTPMessage& msg,
  bool isPushed,
  uint32_t headroom,
  HTTPHeaderSize* size) {

  const HTTPHeaders& headers = msg.getHeaders();
  vector<Header> allHeaders;
  allHeaders.reserve(headers.size() + 6);

  const string& method = msg.getMethodString();
  const string& scheme = msg.isSecure() ? spdy::https : spdy::http;
  string path = msg.getURL();

  if (versionSettings_.majorVersion == 2 && path[0] == '/') {
    // We don't send the host header, SPDY/2 requires absolute URLs
    const string& host = msg.getHeaders().getSingleOrEmpty(HTTP_HEADER_HOST);
    if (!host.empty()) {
      string url = scheme;
      url.append("://");
      url.append(host);
      url.append(path);
      path = std::move(url);
    } // oh well. roll the dice
  }

  if (isPushed) {
    static const string ok("200");
    allHeaders.emplace_back(versionSettings_.statusStr, ok);
  } else {
    allHeaders.emplace_back(versionSettings_.methodStr, method);
  }
  allHeaders.emplace_back(versionSettings_.schemeStr, scheme);
  allHeaders.emplace_back(versionSettings_.pathStr, path);
  if (versionSettings_.majorVersion == 3) {
    DCHECK(headers.exists(HTTP_HEADER_HOST));
    const string& host = headers.getSingleOrEmpty(HTTP_HEADER_HOST);
    allHeaders.emplace_back(versionSettings_.hostStr, host);
}

  return encodeHeaders(msg, allHeaders, headroom, size);
}

void SPDYCodec::generateHeader(folly::IOBufQueue& writeBuf,
                               StreamID stream,
                               const HTTPMessage& msg,
                               StreamID assocStream,
                               HTTPHeaderSize* size) {
  if (transportDirection_ == TransportDirection::UPSTREAM ||
      assocStream != HTTPCodec::NoStream) {
    generateSynStream(stream, assocStream, writeBuf, msg, size);
  } else {
    generateSynReply(stream, writeBuf, msg, size);
  }
}

void SPDYCodec::generateSynStream(StreamID stream,
                                  StreamID assocStream,
                                  folly::IOBufQueue& writeBuf,
                                  const HTTPMessage& msg,
                                  HTTPHeaderSize* size) {
  // Pushed streams must have an even streamId and an odd assocStream
  CHECK((assocStream == HTTPCodec::NoStream && (stream % 2 == 1)) ||
        ((stream % 2 == 0) && (assocStream % 2 == 1))) <<
    "Invalid stream ids stream=" << stream << " assocStream=" << assocStream;

  // Serialize the compressed representation of the headers
  // first because we need to write its length.  The
  // serializeRequestHeaders() method allocates an IOBuf to
  // hold the headers, but we can tell it to reserve
  // enough headroom at the start of the IOBuf to hold
  // the metadata we'll need to add once we know the
  // length.
  uint32_t fieldsSize = kFrameSizeSynStream;
  uint32_t headroom = kFrameSizeControlCommon + fieldsSize;
  bool isPushed = (assocStream != HTTPCodec::NoStream);
  unique_ptr<IOBuf> out(serializeRequestHeaders(msg, isPushed,
                                                headroom, size));

  // The length field in the SYN_STREAM header holds the number
  // of bytes that follow it.  That's the length of the fields
  // specific to the SYN_STREAM message (all of which come after
  // the length field) plus the length of the serialized header
  // name/value block.
  uint32_t len = fieldsSize + out->computeChainDataLength();

  // Generate a control frame header of type SYN_STREAM within
  // the headroom that serializeRequestHeaders() reserved for us
  // at the start of the IOBuf.
  uint8_t flags = (assocStream != HTTPCodec::NoStream) ?
    spdy::CTRL_FLAG_UNIDIRECTIONAL : spdy::CTRL_FLAG_NONE;
  out->prepend(headroom);
  RWPrivateCursor cursor(out.get());
  cursor.writeBE(versionSettings_.controlVersion);
  cursor.writeBE(uint16_t(spdy::SYN_STREAM));
  cursor.writeBE(flagsAndLength(flags, len));
  cursor.writeBE(uint32_t(stream));
  cursor.writeBE(uint32_t(assocStream));
  cursor.writeBE(uint16_t(
                   msg.getPriority() << (versionSettings_.priShift + 8)));

  // Now that we have a complete SYN_STREAM control frame, append
  // it to the writeBuf.
  writeBuf.append(std::move(out));
}

void SPDYCodec::generateSynReply(StreamID stream,
                                 folly::IOBufQueue& writeBuf,
                                 const HTTPMessage& msg,
                                 HTTPHeaderSize* size) {
  // Serialize the compressed representation of the headers
  // first because we need to write its length.  The
  // serializeResponseHeaders() method allocates an IOBuf to
  // hold the headers, but we can tell it to reserve
  // enough headroom at the start of the IOBuf to hold
  // the metadata we'll need to add once we know the
  // length.
  uint32_t headroom = kFrameSizeControlCommon + versionSettings_.synReplySize;
  unique_ptr<IOBuf> out(serializeResponseHeaders(msg, headroom, size));

  // The length field in the SYN_REPLY header holds the number
  // of bytes that follow it.  That's the length of the fields
  // specific to the SYN_REPLY message (all of which come after
  // the length field) plus the length of the serialized header
  // name/value block.
  uint32_t len = versionSettings_.synReplySize + out->computeChainDataLength();

  // Generate a control frame header of type SYN_REPLY within
  // the headroom that we serializeResponseHeaders() reserved for us
  // at the start of the IOBuf.1
  out->prepend(headroom);
  RWPrivateCursor cursor(out.get());
  cursor.writeBE(versionSettings_.controlVersion);
  cursor.writeBE(uint16_t(spdy::SYN_REPLY));
  cursor.writeBE(flagsAndLength(0, len));
  cursor.writeBE(uint32_t(stream)); // TODO: stream should never be bigger than 2^31
  if (versionSettings_.majorVersion == 2) {
    cursor.writeBE(uint16_t(0));
  }

  // Now that we have a complete SYN_REPLY control frame, append
  // it to the writeBuf.
  writeBuf.append(std::move(out));
}

size_t SPDYCodec::generateBody(folly::IOBufQueue& writeBuf,
                               StreamID stream,
                               std::unique_ptr<folly::IOBuf> chain,
                               bool eom) {
  size_t len = chain->computeChainDataLength();
  if (len == 0) {
    return len;
  }

  // TODO if the data length is 2^24 or greater, split it into
  // multiple data frames.  Proxygen should never be writing that
  // much data at once, but other apps that use this codec might.
  CHECK(len < (1 << 24));

  uint8_t flags = (eom) ? kFlagFin : 0;
  generateDataFrame(writeBuf, uint32_t(stream), flags, len);
  writeBuf.append(std::move(chain));
  return len;
}

size_t SPDYCodec::generateChunkHeader(folly::IOBufQueue& writeBuf,
                                      StreamID stream,
                                      size_t length) {
  // SPDY chunk headers are built into the data frames
  return 0;
}

size_t SPDYCodec::generateChunkTerminator(folly::IOBufQueue& writeBuf,
                                          StreamID stream) {
  // SPDY has no chunk terminator
  return 0;
}

size_t SPDYCodec::generateTrailers(folly::IOBufQueue& writeBuf,
                                   StreamID stream,
                                   const HTTPHeaders& trailers) {
  // TODO generate a HEADERS frame?  An additional HEADERS frame
  // somewhere after the SYN_REPLY seems to be the SPDY equivalent
  // of HTTP/1.1's trailers.
  return 0;
}

size_t SPDYCodec::generateEOM(folly::IOBufQueue& writeBuf,
                              StreamID stream) {
  VLOG(4) << "sending EOM for stream=" << stream;
  generateDataFrame(writeBuf, uint32_t(stream), kFlagFin, 0);
  return 8; // size of data frame header
}

size_t SPDYCodec::generateRstStream(IOBufQueue& writeBuf,
                                    StreamID stream,
                                    ErrorCode code) {
  DCHECK(stream > 0);
  VLOG(4) << "sending RST_STREAM for stream=" << stream
          << " with code=" << getErrorCodeString(code);

  // Suppress any EOM callback for the current frame.
  if (stream == streamId_) {
    flags_ &= ~spdy::CTRL_FLAG_FIN;
  }

  const uint32_t statusCode = (uint32_t) spdy::errorCodeToReset(code);
  unique_ptr<IOBuf> frame = IOBuf::create(kFrameSizeControlCommon +
                                          kFrameSizeRstStream);
  size_t written = 0;
  Appender appender(frame.get(), 0);
  appender.writeBE(versionSettings_.controlVersion);
  appender.writeBE(uint16_t(spdy::RST_STREAM));
  appender.writeBE(flagsAndLength(0, kFrameSizeRstStream));
  appender.writeBE(uint32_t(stream));
  appender.writeBE(rstStatusSupported(statusCode) ?
                   statusCode : spdy::RST_PROTOCOL_ERROR);
  written = frame->length();
  writeBuf.append(std::move(frame));
  return written;
}

size_t SPDYCodec::generateGoaway(IOBufQueue& writeBuf,
                                 StreamID lastStream,
                                 ErrorCode code) {
  const uint32_t statusCode = (uint32_t) spdy::errorCodeToGoaway(code);
  unique_ptr<IOBuf> frame = IOBuf::create(kFrameSizeControlCommon +
                                          (size_t)versionSettings_.goawaySize);
  size_t written = 0;
  Appender appender(frame.get(), 0);
  appender.writeBE(versionSettings_.controlVersion);

  if (sessionClosing_ == ClosingState::CLOSING) {
    VLOG(4) << "Not sending GOAWAY for closed session";
    return 0;
  }
  if (code != ErrorCode::NO_ERROR) {
    sessionClosing_ = ClosingState::CLOSING;
  }

  VLOG(4) << "Sending GOAWAY with last acknowledged stream="
          << lastStream << " with code=" << getErrorCodeString(code);

  appender.writeBE(uint16_t(spdy::GOAWAY));
  appender.writeBE(flagsAndLength(0, versionSettings_.goawaySize));
  appender.writeBE(uint32_t(lastStream));
  if (versionSettings_.majorVersion == 3) {
    appender.writeBE(statusCode);
  }
  switch (sessionClosing_) {
    case ClosingState::OPEN:
      sessionClosing_ = ClosingState::CLOSING;
      break;
    case ClosingState::OPEN_WITH_GRACEFUL_DRAIN_ENABLED:
      if (lastStream == std::numeric_limits<int32_t>::max()) {
        sessionClosing_ = ClosingState::FIRST_GOAWAY_SENT;
      } else {
        // The user of this codec decided not to do the double goaway
        // drain
        sessionClosing_ = ClosingState::CLOSING;
      }
      break;
    case ClosingState::FIRST_GOAWAY_SENT:
      sessionClosing_ = ClosingState::CLOSING;
      break;
    case ClosingState::CLOSING:
      break;
  }
  written = frame->length();
  writeBuf.append(std::move(frame));
  return written;
}

size_t SPDYCodec::generatePingRequest(IOBufQueue& writeBuf) {
  const auto id = nextEgressPingID_;
  nextEgressPingID_ += 2;
  VLOG(4) << "Generating ping request with id=" << id;
  return generatePingCommon(writeBuf, id);
}

size_t SPDYCodec::generatePingReply(IOBufQueue& writeBuf, uint64_t uniqueID) {
  VLOG(4) << "Generating ping reply with id=" << uniqueID;
  return generatePingCommon(writeBuf, uniqueID);
}

size_t SPDYCodec::generatePingCommon(IOBufQueue& writeBuf, uint64_t uniqueID) {
  unique_ptr<IOBuf> frame = IOBuf::create(kFrameSizeControlCommon +
                                          kFrameSizePing);
  Appender appender(frame.get(), 0);
  appender.writeBE(versionSettings_.controlVersion);
  appender.writeBE(uint16_t(spdy::PING));
  appender.writeBE(flagsAndLength(0, kFrameSizePing));
  appender.writeBE(uint32_t(uniqueID));
  size_t encodedSize = frame->length();
  writeBuf.append(std::move(frame));
  return encodedSize;
}

size_t SPDYCodec::generateSettings(folly::IOBufQueue& writeBuf) {
  auto numSettings = egressSettings_.getNumSettings();
  VLOG(4) << "generating " << (unsigned)numSettings << " settings";
  unique_ptr<IOBuf> frame = IOBuf::create(
      kFrameSizeControlCommon + kFrameSizeSettings +
      (kFrameSizeSettingsEntry * numSettings));
  Appender appender(frame.get(), 0);
  appender.writeBE(versionSettings_.controlVersion);
  appender.writeBE(uint16_t(spdy::SETTINGS));
  appender.writeBE(flagsAndLength(spdy::FLAG_SETTINGS_CLEAR_SETTINGS,
                                  kFrameSizeSettings +
                                  kFrameSizeSettingsEntry * numSettings));
  appender.writeBE(uint32_t(numSettings));
  for (const auto& setting: egressSettings_.getAllSettings()) {
    if (!setting.isSet) {
      continue;
    }
    auto settingId = spdy::httpToSpdySettingsId(setting.id);
    if (!settingId) {
      LOG(WARNING) << "Invalid SpdySetting " << (uint32_t)setting.id;
      continue;
    }
    VLOG(5) << " writing setting with id=" << *settingId
            << ", value=" << setting.value;
    if (versionSettings_.majorVersion == 2) {
      // ID: 24-bits in little-endian byte order.
      // This is inconsistent with other values in SPDY and
      // is the result of a bug in the initial v2 implementation.
      appender.writeLE(flagsAndLength(0, *settingId));
    } else {
      appender.writeBE(flagsAndLength(0, *settingId));
    }
    appender.writeBE(setting.value);
  }
  size_t written = frame->length();
  writeBuf.append(std::move(frame));
  return written;
}

size_t SPDYCodec::generateWindowUpdate(folly::IOBufQueue& writeBuf,
                                       StreamID stream,
                                       uint32_t delta) {
  if (versionSettings_.majorVersion < 3 ||
      (stream == 0 && versionSettings_.majorVersion == 3 &&
       versionSettings_.minorVersion == 0)) {
    return 0;
  }

  VLOG(4) << "generating window update for stream=" << stream
          << ": Processed " << delta << " bytes";
  unique_ptr<IOBuf> frame = IOBuf::create(kFrameSizeControlCommon +
                                          kFrameSizeWindowUpdate);
  Appender appender(frame.get(), 0);
  appender.writeBE(versionSettings_.controlVersion);
  appender.writeBE(uint16_t(spdy::WINDOW_UPDATE));
  appender.writeBE(flagsAndLength(0, kFrameSizeWindowUpdate));
  appender.writeBE(uint32_t(stream)); // TODO: ensure stream < 2^31
  appender.writeBE(delta); // TODO: delta should never be bigger than 2^31
  size_t written = frame->length();
  writeBuf.append(std::move(frame));
  return written;
}

void SPDYCodec::enableDoubleGoawayDrain() {
  CHECK_EQ(sessionClosing_, ClosingState::OPEN);
  sessionClosing_ = ClosingState::OPEN_WITH_GRACEFUL_DRAIN_ENABLED;
}

uint8_t SPDYCodec::getVersion() const {
  return versionSettings_.majorVersion;
}

uint8_t SPDYCodec::getMinorVersion() const {
  return versionSettings_.minorVersion;
}

size_t SPDYCodec::generateDataFrame(folly::IOBufQueue& writeBuf,
    uint32_t streamID, uint8_t flags, uint32_t length) {
  const size_t frameSize = kFrameSizeDataCommon;
  unique_ptr<IOBuf> frame = IOBuf::create(frameSize);
  frame->append(frameSize);
  RWPrivateCursor cursor(frame.get());
  cursor.writeBE(uint32_t(streamID));
  cursor.writeBE(flagsAndLength(flags, length));
  size_t encodedSize = frame->length();
  writeBuf.append(std::move(frame));
  return encodedSize;
}

unique_ptr<HTTPMessage>
SPDYCodec::parseHeaders(TransportDirection direction, StreamID streamID,
                        StreamID assocStreamID,
                        const HeaderPieceList& inHeaders) {
  unique_ptr<HTTPMessage> msg(new HTTPMessage());
  HTTPHeaders& headers = msg->getHeaders();
  bool newStream = (type_ != spdy::HEADERS);

  bool hasScheme = false;
  bool hasPath = false;

  // Number of fields must be even
  CHECK((inHeaders.size() & 1) == 0);
  for (unsigned i = 0; i < inHeaders.size(); i += 2) {
    uint8_t off = 0;
    uint32_t len = inHeaders[i].str.size();
    if (len > 1 && inHeaders[i].str[0] == ':') {
      off = 1;  // also signals control header
      len--;
    }
    folly::StringPiece name(inHeaders[i].str, off, len);
    folly::StringPiece value = inHeaders[i + 1].str;
    VLOG(5) << "Header " << name << ": " << value;
    bool nameOk = SPDYUtil::validateHeaderName(name);
    bool valueOk = false;
    bool isPath = false;
    bool isMethod = false;
    if (nameOk) {
      if ((version_ == 2 && name == "url") ||
          (version_ == 3 && off && name == "path")) {
        valueOk = SPDYUtil::validateURL(value);
        isPath = true;
        hasPath = true;
      } else if ((version_ == 2 || off) && name == "method") {
        valueOk = SPDYUtil::validateMethod(value);
        isMethod = true;
        if (value == "CONNECT") {
          // We don't support CONNECT request for SPDY
          valueOk = false;
        }
      } else {
        valueOk = SPDYUtil::validateHeaderValue(value, SPDYUtil::STRICT);
      }
    }
    if (!nameOk || !valueOk) {
      if (newStream) {
        if (assocStreamID) {
          callback_->onPushMessageBegin(streamID, assocStreamID, nullptr);
        } else {
          callback_->onMessageBegin(streamID, nullptr);
        }
      }
      headers.add(name, value);
      partialMsg_ = std::move(msg);
      throw SPDYStreamFailed(false, streamID, 400, "Bad header value");
    }
    bool add = false;
    if (off || version_ == 2) {
      if (isMethod) {
        msg->setMethod(value);
      } else if (isPath) {
        msg->setURL(value.str());
      } else if (name == "version") {
        if (caseInsensitiveEqual(value, "http/1.0")) {
          msg->setHTTPVersion(1, 0);
        } else {
          msg->setHTTPVersion(1, 1);
        }
      } else if (version_ == 3 && name == "host") {
        headers.add(HTTP_HEADER_HOST, value.str());
      } else if (name == "scheme") {
        hasScheme = true;
        if (value == "https") {
          msg->setSecure(true);
        }
      } else if (name == "status") {
        if (direction == TransportDirection::UPSTREAM && !assocStreamID) {
          folly::StringPiece codePiece;
          folly::StringPiece reasonPiece;
          if (value.contains(' ')) {
            folly::split<false>(' ', value, codePiece, reasonPiece);
          } else {
            codePiece = value;
          }
          int32_t code = -1;
          try {
            code = folly::to<unsigned int>(codePiece);
          } catch (const std::range_error& ex) {
          }
          if (code >= 100 && code <= 999) {
            msg->setStatusCode(code);
            msg->setStatusMessage(reasonPiece.str());
          } else {
            msg->setStatusCode(0);
            headers.add(name, value);
            partialMsg_ = std::move(msg);
            throw SPDYStreamFailed(newStream, streamID,
                                   spdy::RST_PROTOCOL_ERROR,
                                  "Invalid status code");
          }
        } else if (!assocStreamID) {
          if (version_ == 2) {
            headers.add("Status", value);
          }
        } // else eat the status header because it fails a check in HTTPMessage
      } else if (version_ == 2) {
        add = true;
      }
    } else {
      add = true;
    }
    if (add) {
      if (!inHeaders[i].isMultiValued() && headers.exists(name)) {
        headers.add(name, value);
        partialMsg_ = std::move(msg);
        throw SPDYStreamFailed(newStream, streamID, spdy::RST_PROTOCOL_ERROR,
                               "Duplicate header value");
      }
      headers.add(name, value);
    }
  }
  if (assocStreamID &&
      (!headers.exists(HTTP_HEADER_HOST) || !hasScheme || !hasPath)) {
    // Fail a server push without host, scheme or path headers
    throw SPDYStreamFailed(newStream, streamID, 400, "Bad Request");
  }
  if (direction == TransportDirection::DOWNSTREAM) {
    if (version_ == 2 && !headers.exists(HTTP_HEADER_HOST)) {
      ParseURL url(msg->getURL());
      if (url.valid()) {
        headers.add(HTTP_HEADER_HOST, url.hostAndPort());
      }
    }

    const string& accept_encoding =
      headers.getSingleOrEmpty(HTTP_HEADER_ACCEPT_ENCODING);
    if (accept_encoding.empty()) {
      headers.add(HTTP_HEADER_ACCEPT_ENCODING, "gzip, deflate");
    } else {
      bool hasGzip = false;
      bool hasDeflate = false;
      if (!SPDYUtil::hasGzipAndDeflate(accept_encoding, hasGzip, hasDeflate)) {
        string new_encoding = accept_encoding;
        if (!hasGzip) {
          new_encoding.append(", gzip");
        }
        if (!hasDeflate) {
          new_encoding.append(", deflate");
        }
        headers.set(HTTP_HEADER_ACCEPT_ENCODING, new_encoding);
      }
    }
  }
  return std::move(msg);
}

void SPDYCodec::onSynCommon(StreamID streamID,
                            StreamID assocStreamID,
                            const HeaderPieceList& headers,
                            int8_t pri,
                            const HTTPHeaderSize& size) {
  if (version_ != versionSettings_.majorVersion) {
    LOG(ERROR) << "Invalid version=" << version_;
    throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
  }

  unique_ptr<HTTPMessage> msg = parseHeaders(transportDirection_,
                                             streamID, assocStreamID, headers);
  msg->setIngressHeaderSize(size);

  msg->setSPDY(version_);
  msg->setPriority(pri);
  if (assocStreamID) {
    callback_->onPushMessageBegin(streamID, assocStreamID, msg.get());
  } else {
    callback_->onMessageBegin(streamID, msg.get());
  }

  callback_->onHeadersComplete(streamID, std::move(msg));
}

void SPDYCodec::onSynStream(uint32_t assocStream,
                            uint8_t pri, uint8_t slot,
                            const HeaderPieceList& headers,
                            const HTTPHeaderSize& size) {
  VLOG(4) << "Got SYN_STREAM, stream=" << streamId_
          << " pri=" << folly::to<int>(pri);
  if (printer_) {
    printSynStream(streamId_, assocStream, pri, slot, headers);
  }
  if (sessionClosing_ == ClosingState::CLOSING) {
    VLOG(4) << "Dropping SYN_STREAM after final GOAWAY, stream=" << streamId_;
    // Suppress any EOM callback for the current frame.
    flags_ &= ~spdy::CTRL_FLAG_FIN;
    return;
  }
  if (streamId_ == 0 ||
      streamId_ < lastStreamID_ ||
      (transportDirection_ == TransportDirection::UPSTREAM &&
       (streamId_ & 0x01) == 1) ||
      (transportDirection_ == TransportDirection::DOWNSTREAM &&
       ((streamId_ & 0x1) == 0)) ||
      (transportDirection_ == TransportDirection::UPSTREAM &&
       assocStream == 0)) {
    LOG(ERROR) << " invalid syn stream stream_id=" << streamId_
               << " lastStreamID_=" << lastStreamID_
               << " assocStreamID=" << assocStream
               << " direction=" << transportDirection_;
    throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
  }

  if (streamId_ == lastStreamID_) {
    throw SPDYStreamFailed(true, streamId_, spdy::RST_PROTOCOL_ERROR);
  }
  if (callback_->numIncomingStreams() >=
      egressSettings_.getSetting(SettingsId::MAX_CONCURRENT_STREAMS,
                                 spdy::kMaxConcurrentStreams)) {
    throw SPDYStreamFailed(true, streamId_, spdy::RST_REFUSED_STREAM);
  }
  if (assocStream != 0 && !(flags_ & spdy::CTRL_FLAG_UNIDIRECTIONAL)) {
    throw SPDYStreamFailed(true, streamId_, spdy::RST_PROTOCOL_ERROR);
  }
  lastStreamID_ = streamId_;
  onSynCommon(StreamID(streamId_),
              StreamID(assocStream), headers, pri, size);
}

void SPDYCodec::onSynReply(const HeaderPieceList& headers,
                           const HTTPHeaderSize& size) {
  VLOG(4) << "Got SYN_REPLY, stream=" << streamId_;
  if (printer_) {
    printSynReply(streamId_, headers);
  }
  if (transportDirection_ == TransportDirection::DOWNSTREAM ||
      (streamId_ & 0x1) == 0) {
    throw SPDYStreamFailed(true, streamId_, spdy::RST_PROTOCOL_ERROR);
  }
  // Server push transactions, short of any better heuristics,
  // should have a background priority. Thus, we pick the largest
  // numerical value for the SPDY priority, which no matter what
  // protocol version this is can be conveyed to onSynCommon by -1.
  onSynCommon(StreamID(streamId_),
              HTTPCodec::NoStream, headers, -1, size);
}

void SPDYCodec::onRstStream(uint32_t statusCode) noexcept {
  VLOG(4) << "Got RST_STREAM, stream=" << streamId_
          << ", status=" << statusCode;
  if (printer_) {
    printRstStream(streamId_, statusCode);
  }
  StreamID streamID(streamId_);
  callback_->onAbort(streamID,
                     spdy::rstToErrorCode(spdy::ResetStatusCode(statusCode)));
}

void SPDYCodec::onSettings(const SettingList& settings) {
  VLOG(4) << "Got " << settings.size() << " settings with "
          << "version=" << version_ << " and flags="
          << std::hex << folly::to<unsigned int>(flags_) << std::dec;
  if (printer_) {
    printSettings(settings);
  }
  SettingsList settingsList;
  for (const SettingData& cur: settings) {
    // For now, we never ask for anything to be persisted, so ignore anything
    // coming back
    if (cur.flags & spdy::ID_FLAG_SETTINGS_PERSISTED) {
      VLOG(2) << "Ignoring bogus persisted setting: " << cur.id;
      continue;
    }

    switch (cur.id) {
      case spdy::SETTINGS_UPLOAD_BANDWIDTH:
      case spdy::SETTINGS_DOWNLOAD_BANDWIDTH:
      case spdy::SETTINGS_ROUND_TRIP_TIME:
      case spdy::SETTINGS_CURRENT_CWND:
      case spdy::SETTINGS_DOWNLOAD_RETRANS_RATE:
      case spdy::SETTINGS_CLIENT_CERTIFICATE_VECTOR_SIZE:
        // These will be stored in ingressSettings_ and passed to the callback
        // but we currently ignore the PERSIST flag
        break;
      case spdy::SETTINGS_MAX_CONCURRENT_STREAMS:
        break;
      case spdy::SETTINGS_INITIAL_WINDOW_SIZE:
        if (cur.value > std::numeric_limits<int32_t>::max()) {
          throw SPDYSessionFailed(spdy::GOAWAY_PROTOCOL_ERROR);
        }
        break;
      default:
        LOG(ERROR) << "Received unknown setting with ID=" << cur.id
                   << ", value=" << cur.value
                   << ", and flags=" << std::hex << cur.flags << std::dec;
    }
    auto id = spdy::spdyToHttpSettingsId((spdy::SettingsId)cur.id);
    if (id) {
      ingressSettings_.setSetting(*id, cur.value);
      auto s = ingressSettings_.getSetting(*id);
      settingsList.push_back(*s);
    }
  }
  callback_->onSettings(settingsList);
}

void SPDYCodec::onPing(uint32_t uniqueID) noexcept {
  if (printer_) {
    printPing(uniqueID);
  }
  bool odd = uniqueID & 0x1;
  bool isReply = true;
  if (transportDirection_ == TransportDirection::DOWNSTREAM) {
    if (odd) {
      isReply = false;
    }
  } else if (!odd) {
    isReply = false;
  }

  if (isReply) {
    if (uniqueID >= nextEgressPingID_) {
      LOG(INFO) << "Received reply for pingID=" << uniqueID
                << " that was never sent";
      return;
    }
    callback_->onPingReply(uniqueID);
  } else {
    callback_->onPingRequest(uniqueID);
  }
}

void SPDYCodec::onGoaway(uint32_t lastGoodStream,
                         uint32_t statusCode) noexcept {
  if (printer_) {
    printGoaway(lastGoodStream, statusCode);
  }
  VLOG(4) << "Got GOAWAY, lastGoodStream=" << lastGoodStream
          << ", statusCode=" << statusCode;

  if (lastGoodStream < ingressGoawayAck_) {
    ingressGoawayAck_ = lastGoodStream;
    // Drain all streams <= lastGoodStream
    // and abort streams > lastGoodStream
    callback_->onGoaway(lastGoodStream, spdy::goawayToErrorCode(
                          spdy::GoawayStatusCode(statusCode)));
  } else {
    LOG(WARNING) << "Received multiple GOAWAY with increasing ack";
  }
}

void SPDYCodec::onHeaders(const HeaderPieceList& headers) noexcept {
  if (printer_) {
    printHeaders(streamId_, headers);
  }
  VLOG(3) << "onHeaders is unimplemented.";
}

void SPDYCodec::onWindowUpdate(uint32_t delta) noexcept {
  if (printer_) {
    printWindowUpdate(streamId_, delta);
  }
  callback_->onWindowUpdate(streamId_, delta);
}

void SPDYCodec::failStream(bool newStream, StreamID streamID,
                           uint32_t code, string excStr) {
  // Suppress any EOM callback for the current frame.
  if (streamID == streamId_) {
    flags_ &= ~spdy::CTRL_FLAG_FIN;
  }

  HTTPException err(
    code >= 100 ?
    HTTPException::Direction::INGRESS :
    HTTPException::Direction::INGRESS_AND_EGRESS,
    "SPDYCodec stream error: stream=",
    streamID, " status=", code, " exception: ", excStr);
  if (code >= 100) {
    err.setHttpStatusCode(code);
  } else {
    err.setCodecStatusCode(spdy::rstToErrorCode(spdy::ResetStatusCode(code)));
  }
  err.setProxygenError(kErrorParseHeader);

  if (partialMsg_) {
    err.setPartialMsg(std::move(partialMsg_));
  }
  // store the ingress buffer
  if (currentIngressBuf_) {
    err.setCurrentIngressBuf(std::move(currentIngressBuf_->clone()));
  }
  callback_->onError(streamID, err, newStream);
}

void SPDYCodec::failSession(uint32_t code) {
  HTTPException err(
    HTTPException::Direction::INGRESS_AND_EGRESS,
    "SPDYCodec session error "
    "lastGoodStream=", lastStreamID_, " status=", code);
  err.setCodecStatusCode(spdy::goawayToErrorCode(spdy::GoawayStatusCode(code)));
  err.setProxygenError(kErrorParseHeader);

  // store the ingress buffer
  if (currentIngressBuf_) {
    err.setCurrentIngressBuf(std::move(currentIngressBuf_->clone()));
  }
  callback_->onError(0, err);
}

bool SPDYCodec::rstStatusSupported(int statusCode) const {
  if (statusCode == 0) {
    // 0 is not a valid status code for RST_STREAM
    return false;
  }
  // SPDY/3 supports more status codes for RST_STREAM. For SPDY/2,
  // we just use PROTOCOL_ERROR for these new higher numbered error codes.
  return (versionSettings_.majorVersion != 2 ||
          statusCode <= spdy::RST_FLOW_CONTROL_ERROR);
}

boost::optional<SPDYVersion>
SPDYCodec::getVersion(const std::string& protocol) {
  // Fail fast if it's not possible for the protocol string to define a
  // SPDY protocol. strlen("spdy/1") == 6
  if (protocol.length() < 6) {
    return boost::none;
  }

  if (protocol == kHpackNpn) {
    return SPDYVersion::SPDY3_1_HPACK;
  }
  if (protocol == "spdy/3.1") {
    return SPDYVersion::SPDY3_1;
  }
  if (protocol == "spdy/3") {
    return SPDYVersion::SPDY3;
  }
  if (protocol == "spdy/2") {
    return SPDYVersion::SPDY2;
  }

  return boost::none;
}

}