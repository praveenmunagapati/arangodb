////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "VelocyPackDumper.h"
#include "Basics/Exceptions.h"
#include "Basics/fpconv.h"
#include "Logger/Logger.h"
//#include "Basics/StringUtils.h"
//#include "Basics/Utf8Helper.h"
//#include "Basics/VPackStringBufferAdapter.h"
//#include "Basics/files.h"
//#include "Basics/hashes.h"
//#include "Basics/tri-strings.h"

#include <velocypack/velocypack-common.h>
#include <velocypack/AttributeTranslator.h>
#include "velocypack/Iterator.h"
#include <velocypack/Options.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb::basics;
  
void VelocyPackDumper::handleUnsupportedType(VPackSlice const* slice) {
  if (options->unsupportedTypeBehavior == VPackOptions::NullifyUnsupportedType) {
    _buffer->appendText("null", 4);
    return;
  } else if (options->unsupportedTypeBehavior == VPackOptions::ConvertUnsupportedType) {
    _buffer->appendText(std::string("\"(non-representable type ") + slice->typeName() + ")\"");
    return;
  }

  throw VPackException(VPackException::NoJsonEquivalent);
}
  
void VelocyPackDumper::appendUInt(uint64_t v) {
  if (10000000000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 10000000000000000000ULL) % 10);
  }
  if (1000000000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 1000000000000000000ULL) % 10);
  }
  if (100000000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 100000000000000000ULL) % 10);
  }
  if (10000000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 10000000000000000ULL) % 10);
  }
  if (1000000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 1000000000000000ULL) % 10);
  }
  if (100000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 100000000000000ULL) % 10);
  }
  if (10000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 10000000000000ULL) % 10);
  }
  if (1000000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 1000000000000ULL) % 10);
  }
  if (100000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 100000000000ULL) % 10);
  }
  if (10000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 10000000000ULL) % 10);
  }
  if (1000000000ULL <= v) {
    _buffer->appendChar('0' + (v / 1000000000ULL) % 10);
  }
  if (100000000ULL <= v) {
    _buffer->appendChar('0' + (v / 100000000ULL) % 10);
  }
  if (10000000ULL <= v) {
    _buffer->appendChar('0' + (v / 10000000ULL) % 10);
  }
  if (1000000ULL <= v) {
    _buffer->appendChar('0' + (v / 1000000ULL) % 10);
  }
  if (100000ULL <= v) {
    _buffer->appendChar('0' + (v / 100000ULL) % 10);
  }
  if (10000ULL <= v) {
    _buffer->appendChar('0' + (v / 10000ULL) % 10);
  }
  if (1000ULL <= v) {
    _buffer->appendChar('0' + (v / 1000ULL) % 10);
  }
  if (100ULL <= v) {
    _buffer->appendChar('0' + (v / 100ULL) % 10);
  }
  if (10ULL <= v) {
    _buffer->appendChar('0' + (v / 10ULL) % 10);
  }

  _buffer->appendChar('0' + (v % 10));
}

void VelocyPackDumper::appendDouble(double v) {
  char temp[24];
  int len = fpconv_dtoa(v, &temp[0]);
  _buffer->appendText(&temp[0], static_cast<VPackValueLength>(len));
}

void VelocyPackDumper::dumpInteger(VPackSlice const* slice) {
  if (slice->isType(VPackValueType::UInt)) {
    uint64_t v = slice->getUInt();

    appendUInt(v);
  } else if (slice->isType(VPackValueType::Int)) {
    int64_t v = slice->getInt();
    if (v == INT64_MIN) {
      _buffer->appendText("-9223372036854775808", 20);
      return;
    }
    if (v < 0) {
      _buffer->appendChar('-');
      v = -v;
    }

    if (1000000000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 1000000000000000000LL) % 10);
    }
    if (100000000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 100000000000000000LL) % 10);
    }
    if (10000000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 10000000000000000LL) % 10);
    }
    if (1000000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 1000000000000000LL) % 10);
    }
    if (100000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 100000000000000LL) % 10);
    }
    if (10000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 10000000000000LL) % 10);
    }
    if (1000000000000LL <= v) {
      _buffer->appendChar('0' + (v / 1000000000000LL) % 10);
    }
    if (100000000000LL <= v) {
      _buffer->appendChar('0' + (v / 100000000000LL) % 10);
    }
    if (10000000000LL <= v) {
      _buffer->appendChar('0' + (v / 10000000000LL) % 10);
    }
    if (1000000000LL <= v) {
      _buffer->appendChar('0' + (v / 1000000000LL) % 10);
    }
    if (100000000LL <= v) {
      _buffer->appendChar('0' + (v / 100000000LL) % 10);
    }
    if (10000000LL <= v) {
      _buffer->appendChar('0' + (v / 10000000LL) % 10);
    }
    if (1000000LL <= v) {
      _buffer->appendChar('0' + (v / 1000000LL) % 10);
    }
    if (100000LL <= v) {
      _buffer->appendChar('0' + (v / 100000LL) % 10);
    }
    if (10000LL <= v) {
      _buffer->appendChar('0' + (v / 10000LL) % 10);
    }
    if (1000LL <= v) {
      _buffer->appendChar('0' + (v / 1000LL) % 10);
    }
    if (100LL <= v) {
      _buffer->appendChar('0' + (v / 100LL) % 10);
    }
    if (10LL <= v) {
      _buffer->appendChar('0' + (v / 10LL) % 10);
    }

    _buffer->appendChar('0' + (v % 10));
  } else if (slice->isType(VPackValueType::SmallInt)) {
    int64_t v = slice->getSmallInt();
    if (v < 0) {
      _buffer->appendChar('-');
      v = -v;
    }
    _buffer->appendChar('0' + static_cast<char>(v));
  }
}

void VelocyPackDumper::dumpString(char const* src, VPackValueLength len) {
  static char const EscapeTable[256] = {
      // 0    1    2    3    4    5    6    7    8    9    A    B    C    D    E
      // F
      'u',  'u', 'u', 'u', 'u', 'u', 'u', 'u', 'b', 't', 'n', 'u', 'f', 'r',
      'u',
      'u',  // 00
      'u',  'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
      'u',
      'u',  // 10
      0,    0,   '"', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,
      '/',  // 20
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,
      0,  // 30~4F
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      '\\', 0,   0,   0,  // 50
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,
      0,  // 60~FF
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,    0,   0,   0};

  _buffer->reserve(len);

  uint8_t const* p = reinterpret_cast<uint8_t const*>(src);
  uint8_t const* e = p + len;
  while (p < e) {
    uint8_t c = *p;

    if ((c & 0x80) == 0) {
      // check for control characters
      char esc = EscapeTable[c];

      if (esc) {
        if (c != '/' || options->escapeForwardSlashes) {
          // escape forward slashes only when requested
          _buffer->appendChar('\\');
        }
        _buffer->appendChar(static_cast<char>(esc));

        if (esc == 'u') {
          uint16_t i1 = (((uint16_t)c) & 0xf0) >> 4;
          uint16_t i2 = (((uint16_t)c) & 0x0f);

          _buffer->appendText("00", 2);
          _buffer->appendChar(
              static_cast<char>((i1 < 10) ? ('0' + i1) : ('A' + i1 - 10)));
          _buffer->appendChar(
              static_cast<char>((i2 < 10) ? ('0' + i2) : ('A' + i2 - 10)));
        }
      } else {
        _buffer->appendChar(static_cast<char>(c));
      }
    } else if ((c & 0xe0) == 0xc0) {
      // two-byte sequence
      if (p + 1 >= e) {
        throw VPackException(VPackException::InvalidUtf8Sequence);
      }

      _buffer->appendText(reinterpret_cast<char const*>(p), 2);
      ++p;
    } else if ((c & 0xf0) == 0xe0) {
      // three-byte sequence
      if (p + 2 >= e) {
        throw VPackException(VPackException::InvalidUtf8Sequence);
      }

      _buffer->appendText(reinterpret_cast<char const*>(p), 3);
      p += 2;
    } else if ((c & 0xf8) == 0xf0) {
      // four-byte sequence
      if (p + 3 >= e) {
        throw VPackException(VPackException::InvalidUtf8Sequence);
      }

      _buffer->appendText(reinterpret_cast<char const*>(p), 4);
      p += 3;
    }

    ++p;
  }
}

void VelocyPackDumper::dumpValue(VPackSlice const* slice, VPackSlice const* base) {
  if (base == nullptr) {
    base = slice;
  }

  switch (slice->type()) {
    case VPackValueType::Null: {
      _buffer->appendText("null", 4);
      break;
    }

    case VPackValueType::Bool: {
      if (slice->getBool()) {
        _buffer->appendText("true", 4);
      } else {
        _buffer->appendText("false", 5);
      }
      break;
    }

    case VPackValueType::Array: {
      VPackArrayIterator it(*slice, true);
      _buffer->appendChar('[');
      while (it.valid()) {
        if (!it.isFirst()) {
          _buffer->appendChar(',');
        }
        dumpValue(it.value(), slice);
        it.next();
      }
      _buffer->appendChar(']');
      break;
    }

    case VPackValueType::Object: {
      VPackObjectIterator it(*slice);
      _buffer->appendChar('{');
      while (it.valid()) {
        if (!it.isFirst()) {
          _buffer->appendChar(',');
        }
        dumpValue(it.key().makeKey(), slice);
        _buffer->appendChar(':');
        dumpValue(it.value(), slice);
        it.next();
      }
      _buffer->appendChar('}');
      break;
    }

    case VPackValueType::Double: {
      double const v = slice->getDouble();
      if (std::isnan(v) || !std::isfinite(v)) {
        handleUnsupportedType(slice);
      } else {
        appendDouble(v);
      }
      break;
    }

    case VPackValueType::Int:
    case VPackValueType::UInt:
    case VPackValueType::SmallInt: {
      dumpInteger(slice);
      break;
    }

    case VPackValueType::String: {
      VPackValueLength len;
      char const* p = slice->getString(len);
      _buffer->reserve(2 + len);
      _buffer->appendChar('"');
      dumpString(p, len);
      _buffer->appendChar('"');
      break;
    }
    
    case VPackValueType::External: {
      VPackSlice const external(slice->getExternal());
      dumpValue(&external, base);
      break;
    }
    
    case VPackValueType::Custom: {
      if (options->customTypeHandler == nullptr) {
        throw VPackException(VPackException::NeedCustomTypeHandler);
      } else {
        std::string v = options->customTypeHandler->toString(*slice, nullptr, *base);
        dumpString(v.c_str(), v.size());
      }
      break;
    }

    case VPackValueType::UTCDate: 
    case VPackValueType::None: 
    case VPackValueType::Binary: 
    case VPackValueType::Illegal:
    case VPackValueType::MinKey:
    case VPackValueType::MaxKey: 
    case VPackValueType::BCD: {
      handleUnsupportedType(slice);
      break;
    }

  }
}

