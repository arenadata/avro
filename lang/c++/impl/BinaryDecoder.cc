/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Decoder.hh"
#include "Exception.hh"
#include "Zigzag.hh"
#include <memory>

namespace avro {

using std::make_shared;

class BinaryDecoder : public Decoder {
    StreamReader in_;

    void init(InputStream &is) override;
    void decodeNull() override;
    bool decodeBool() override;
    int32_t decodeInt() override;
    int64_t decodeLong() override;
    float decodeFloat() override;
    double decodeDouble() override;
    void decodeString(std::string &value) override;
    void skipString() override;
    void decodeBytes(std::vector<uint8_t> &value) override;
    void skipBytes() override;
    void decodeFixed(size_t n, std::vector<uint8_t> &value) override;
    void skipFixed(size_t n) override;
    size_t decodeEnum() override;
    size_t arrayStart() override;
    size_t arrayNext() override;
    size_t skipArray() override;
    size_t mapStart() override;
    size_t mapNext() override;
    size_t skipMap() override;
    size_t decodeUnionIndex() override;

    int64_t doDecodeLong();
    size_t doDecodeItemCount();
    size_t doDecodeLength();
    void drain() override;
};

DecoderPtr binaryDecoder() {
    return make_shared<BinaryDecoder>();
}

void BinaryDecoder::init(InputStream &is) {
    in_.reset(is);
}

void BinaryDecoder::decodeNull() {
}

bool BinaryDecoder::decodeBool() {
    auto v = in_.read();
    if (v == 0) {
        return false;
    } else if (v == 1) {
        return true;
    }
    throw Exception(boost::format("Invalid value for bool: %1%") % v);
}

int32_t BinaryDecoder::decodeInt() {
    auto val = doDecodeLong();
    if (val < INT32_MIN || val > INT32_MAX) {
        throw Exception(
            boost::format("Value out of range for Avro int: %1%") % val);
    }
    return static_cast<int32_t>(val);
}

int64_t BinaryDecoder::decodeLong() {
    return doDecodeLong();
}

float BinaryDecoder::decodeFloat() {
    float result;
    in_.readBytes(reinterpret_cast<uint8_t *>(&result), sizeof(float));
    return result;
}

double BinaryDecoder::decodeDouble() {
    double result;
    in_.readBytes(reinterpret_cast<uint8_t *>(&result), sizeof(double));
    return result;
}

size_t BinaryDecoder::doDecodeLength() {
    ssize_t len = decodeInt();
    if (len < 0) {
        throw Exception(
            boost::format("Cannot have negative length: %1%") % len);
    }
    return len;
}

void BinaryDecoder::drain() {
    in_.drain(false);
}

void BinaryDecoder::decodeString(std::string &value) {
    size_t len = doDecodeLength();
    value.resize(len);
    if (len > 0) {
        in_.readBytes(const_cast<uint8_t *>(
                          reinterpret_cast<const uint8_t *>(value.c_str())),
                      len);
    }
}

void BinaryDecoder::skipString() {
    size_t len = doDecodeLength();
    in_.skipBytes(len);
}

void BinaryDecoder::decodeBytes(std::vector<uint8_t> &value) {
    size_t len = doDecodeLength();
    value.resize(len);
    if (len > 0) {
        in_.readBytes(value.data(), len);
    }
}

void BinaryDecoder::skipBytes() {
    size_t len = doDecodeLength();
    in_.skipBytes(len);
}

void BinaryDecoder::decodeFixed(size_t n, std::vector<uint8_t> &value) {
    value.resize(n);
    if (n > 0) {
        in_.readBytes(value.data(), n);
    }
}

void BinaryDecoder::skipFixed(size_t n) {
    in_.skipBytes(n);
}

size_t BinaryDecoder::decodeEnum() {
    return static_cast<size_t>(doDecodeLong());
}

size_t BinaryDecoder::arrayStart() {
    return doDecodeItemCount();
}

size_t BinaryDecoder::doDecodeItemCount() {
    auto result = doDecodeLong();
    if (result < 0) {
        doDecodeLong();
        return static_cast<size_t>(-result);
    }
    return static_cast<size_t>(result);
}

size_t BinaryDecoder::arrayNext() {
    return static_cast<size_t>(doDecodeLong());
}

size_t BinaryDecoder::skipArray() {
    for (;;) {
        auto r = doDecodeLong();
        if (r < 0) {
            auto n = static_cast<size_t>(doDecodeLong());
            in_.skipBytes(n);
        } else {
            return static_cast<size_t>(r);
        }
    }
}

size_t BinaryDecoder::mapStart() {
    return doDecodeItemCount();
}

size_t BinaryDecoder::mapNext() {
    return doDecodeItemCount();
}

size_t BinaryDecoder::skipMap() {
    return skipArray();
}

size_t BinaryDecoder::decodeUnionIndex() {
    return static_cast<size_t>(doDecodeLong());
}

int64_t BinaryDecoder::doDecodeLong() {
    uint64_t encoded = 0;
    int shift = 0;
    uint8_t u;
    do {
        if (shift >= 64) {
            throw Exception("Invalid Avro varint");
        }
        u = in_.read();
        encoded |= static_cast<uint64_t>(u & 0x7f) << shift;
        shift += 7;
    } while (u & 0x80);

    return decodeZigzag64(encoded);
}

} // namespace avro
