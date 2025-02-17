/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.NumberFormat implementation. */

#include "builtin/intl/NumberFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/intl/NumberFormat.h"
#include "mozilla/intl/NumberRangeFormat.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"
#include "mozilla/UniquePtr.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <type_traits>

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/DecimalNumber.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/MeasureUnitGenerated.h"
#include "builtin/intl/RelativeTimeFormat.h"
#include "builtin/intl/ScopedICUObject.h"
#include "ds/Sort.h"
#include "gc/FreeOp.h"
#include "js/CharacterEncoding.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"
#include "unicode/udata.h"
#include "unicode/ufieldpositer.h"
#include "unicode/uformattedvalue.h"
#include "unicode/unum.h"
#include "unicode/unumberformatter.h"
#include "unicode/unumsys.h"
#include "unicode/ures.h"
#include "unicode/utypes.h"
#include "util/Text.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SelfHosting.h"
#include "vm/Stack.h"
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::AssertedCast;
using mozilla::IsFinite;
using mozilla::IsNaN;
using mozilla::IsNegative;
using mozilla::SpecificNaN;

using js::intl::CallICU;
using js::intl::DateTimeFormatOptions;
using js::intl::FieldType;
using js::intl::IcuLocale;

const JSClassOps NumberFormatObject::classOps_ = {
    nullptr,                       // addProperty
    nullptr,                       // delProperty
    nullptr,                       // enumerate
    nullptr,                       // newEnumerate
    nullptr,                       // resolve
    nullptr,                       // mayResolve
    NumberFormatObject::finalize,  // finalize
    nullptr,                       // call
    nullptr,                       // hasInstance
    nullptr,                       // construct
    nullptr,                       // trace
};

const JSClass NumberFormatObject::class_ = {
    "Intl.NumberFormat",
    JSCLASS_HAS_RESERVED_SLOTS(NumberFormatObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_NumberFormat) |
        JSCLASS_FOREGROUND_FINALIZE,
    &NumberFormatObject::classOps_, &NumberFormatObject::classSpec_};

const JSClass& NumberFormatObject::protoClass_ = PlainObject::class_;

static bool numberFormat_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().NumberFormat);
  return true;
}

static const JSFunctionSpec numberFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_NumberFormat_supportedLocalesOf", 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec numberFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_NumberFormat_resolvedOptions", 0,
                      0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_NumberFormat_formatToParts", 1, 0),
#ifdef NIGHTLY_BUILD
#  ifdef MOZ_INTL_HAS_NUMBER_RANGE_FORMAT
    JS_SELF_HOSTED_FN("formatRange", "Intl_NumberFormat_formatRange", 2, 0),
    JS_SELF_HOSTED_FN("formatRangeToParts",
                      "Intl_NumberFormat_formatRangeToParts", 2, 0),
#  endif
#endif
    JS_FN(js_toSource_str, numberFormat_toSource, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec numberFormat_properties[] = {
    JS_SELF_HOSTED_GET("format", "$Intl_NumberFormat_format_get", 0),
    JS_STRING_SYM_PS(toStringTag, "Intl.NumberFormat", JSPROP_READONLY),
    JS_PS_END,
};

static bool NumberFormat(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec NumberFormatObject::classSpec_ = {
    GenericCreateConstructor<NumberFormat, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<NumberFormatObject>,
    numberFormat_static_methods,
    nullptr,
    numberFormat_methods,
    numberFormat_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

/**
 * 11.2.1 Intl.NumberFormat([ locales [, options]])
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
static bool NumberFormat(JSContext* cx, const CallArgs& args, bool construct) {
  // Step 1 (Handled by OrdinaryCreateFromConstructor fallback code).

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_NumberFormat,
                                          &proto)) {
    return false;
  }

  Rooted<NumberFormatObject*> numberFormat(cx);
  numberFormat = NewObjectWithClassProto<NumberFormatObject>(cx, proto);
  if (!numberFormat) {
    return false;
  }

  RootedValue thisValue(cx,
                        construct ? ObjectValue(*numberFormat) : args.thisv());
  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 3.
  return intl::LegacyInitializeObject(
      cx, numberFormat, cx->names().InitializeNumberFormat, thisValue, locales,
      options, DateTimeFormatOptions::Standard, args.rval());
}

static bool NumberFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return NumberFormat(cx, args, args.isConstructing());
}

bool js::intl_NumberFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(!args.isConstructing());
  // intl_NumberFormat is an intrinsic for self-hosted JavaScript, so it
  // cannot be used with "new", but it still has to be treated as a
  // constructor.
  return NumberFormat(cx, args, true);
}

void js::NumberFormatObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());

  auto* numberFormat = &obj->as<NumberFormatObject>();
  mozilla::intl::NumberFormat* nf = numberFormat->getNumberFormatter();
  mozilla::intl::NumberRangeFormat* nrf =
      numberFormat->getNumberRangeFormatter();

  if (nf) {
    intl::RemoveICUCellMemory(fop, obj, NumberFormatObject::EstimatedMemoryUse);
    // This was allocated using `new` in mozilla::intl::NumberFormat, so we
    // delete here.
    delete nf;
  }

  if (nrf) {
    intl::RemoveICUCellMemory(fop, obj, EstimatedRangeFormatterMemoryUse);
    // This was allocated using `new` in mozilla::intl::NumberRangeFormat, so we
    // delete here.
    delete nrf;
  }
}

bool js::intl_numberingSystem(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  UErrorCode status = U_ZERO_ERROR;
  UNumberingSystem* numbers = unumsys_open(IcuLocale(locale.get()), &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  ScopedICUObject<UNumberingSystem, unumsys_close> toClose(numbers);

  const char* name = unumsys_getName(numbers);
  if (!name) {
    intl::ReportInternalError(cx);
    return false;
  }

  JSString* jsname = NewStringCopyZ<CanGC>(cx, name);
  if (!jsname) {
    return false;
  }

  args.rval().setString(jsname);
  return true;
}

#if DEBUG || MOZ_SYSTEM_ICU
class UResourceBundleDeleter {
 public:
  void operator()(UResourceBundle* aPtr) { ures_close(aPtr); }
};

using UniqueUResourceBundle =
    mozilla::UniquePtr<UResourceBundle, UResourceBundleDeleter>;

bool js::intl_availableMeasurementUnits(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  RootedObject measurementUnits(
      cx, NewObjectWithGivenProto<PlainObject>(cx, nullptr));
  if (!measurementUnits) {
    return false;
  }

  // Lookup the available measurement units in the resource boundle of the root
  // locale.

  static const char packageName[] =
      U_ICUDATA_NAME U_TREE_SEPARATOR_STRING "unit";
  static const char rootLocale[] = "";

  UErrorCode status = U_ZERO_ERROR;
  UResourceBundle* rawRes = ures_open(packageName, rootLocale, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  UniqueUResourceBundle res(rawRes);

  UResourceBundle* rawUnits =
      ures_getByKey(res.get(), "units", nullptr, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  UniqueUResourceBundle units(rawUnits);

  RootedAtom unitAtom(cx);

  int32_t unitsSize = ures_getSize(units.get());
  for (int32_t i = 0; i < unitsSize; i++) {
    UResourceBundle* rawType =
        ures_getByIndex(units.get(), i, nullptr, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }
    UniqueUResourceBundle type(rawType);

    int32_t typeSize = ures_getSize(type.get());
    for (int32_t j = 0; j < typeSize; j++) {
      UResourceBundle* rawSubtype =
          ures_getByIndex(type.get(), j, nullptr, &status);
      if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
      }
      UniqueUResourceBundle subtype(rawSubtype);

      const char* unitIdentifier = ures_getKey(subtype.get());

      unitAtom = Atomize(cx, unitIdentifier, strlen(unitIdentifier));
      if (!unitAtom) {
        return false;
      }
      if (!DefineDataProperty(cx, measurementUnits, unitAtom->asPropertyName(),
                              TrueHandleValue)) {
        return false;
      }
    }
  }

  args.rval().setObject(*measurementUnits);
  return true;
}
#endif

static constexpr size_t MaxUnitLength() {
  size_t length = 0;
  for (const auto& unit : simpleMeasureUnits) {
    length = std::max(length, std::char_traits<char>::length(unit.name));
  }
  return length * 2 + std::char_traits<char>::length("-per-");
}

static UniqueChars NumberFormatLocale(JSContext* cx, HandleObject internals) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return nullptr;
  }

  // ICU expects numberingSystem as a Unicode locale extensions on locale.

  intl::LanguageTag tag(cx);
  {
    JSLinearString* locale = value.toString()->ensureLinear(cx);
    if (!locale) {
      return nullptr;
    }

    if (!intl::LanguageTagParser::parse(cx, locale, tag)) {
      return nullptr;
    }
  }

  JS::RootedVector<intl::UnicodeExtensionKeyword> keywords(cx);

  if (!GetProperty(cx, internals, internals, cx->names().numberingSystem,
                   &value)) {
    return nullptr;
  }

  {
    JSLinearString* numberingSystem = value.toString()->ensureLinear(cx);
    if (!numberingSystem) {
      return nullptr;
    }

    if (!keywords.emplaceBack("nu", numberingSystem)) {
      return nullptr;
    }
  }

  // |ApplyUnicodeExtensionToTag| applies the new keywords to the front of
  // the Unicode extension subtag. We're then relying on ICU to follow RFC
  // 6067, which states that any trailing keywords using the same key
  // should be ignored.
  if (!intl::ApplyUnicodeExtensionToTag(cx, tag, keywords)) {
    return nullptr;
  }

  return tag.toStringZ(cx);
}

struct NumberFormatOptions : public mozilla::intl::NumberRangeFormatOptions {
  static_assert(std::is_base_of_v<mozilla::intl::NumberFormatOptions,
                                  mozilla::intl::NumberRangeFormatOptions>);

  char currencyChars[3] = {};
  char unitChars[MaxUnitLength()] = {};
};

static bool FillNumberFormatOptions(JSContext* cx, HandleObject internals,
                                    NumberFormatOptions& options) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().style, &value)) {
    return false;
  }

  bool accountingSign = false;
  {
    JSLinearString* style = value.toString()->ensureLinear(cx);
    if (!style) {
      return false;
    }

    if (StringEqualsLiteral(style, "currency")) {
      if (!GetProperty(cx, internals, internals, cx->names().currency,
                       &value)) {
        return false;
      }
      JSLinearString* currency = value.toString()->ensureLinear(cx);
      if (!currency) {
        return false;
      }

      MOZ_RELEASE_ASSERT(
          currency->length() == 3,
          "IsWellFormedCurrencyCode permits only length-3 strings");
      MOZ_ASSERT(StringIsAscii(currency),
                 "IsWellFormedCurrencyCode permits only ASCII strings");
      CopyChars(reinterpret_cast<Latin1Char*>(options.currencyChars),
                *currency);

      if (!GetProperty(cx, internals, internals, cx->names().currencyDisplay,
                       &value)) {
        return false;
      }
      JSLinearString* currencyDisplay = value.toString()->ensureLinear(cx);
      if (!currencyDisplay) {
        return false;
      }

      using CurrencyDisplay =
          mozilla::intl::NumberFormatOptions::CurrencyDisplay;

      CurrencyDisplay display;
      if (StringEqualsLiteral(currencyDisplay, "code")) {
        display = CurrencyDisplay::Code;
      } else if (StringEqualsLiteral(currencyDisplay, "symbol")) {
        display = CurrencyDisplay::Symbol;
      } else if (StringEqualsLiteral(currencyDisplay, "narrowSymbol")) {
        display = CurrencyDisplay::NarrowSymbol;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(currencyDisplay, "name"));
        display = CurrencyDisplay::Name;
      }

      if (!GetProperty(cx, internals, internals, cx->names().currencySign,
                       &value)) {
        return false;
      }
      JSLinearString* currencySign = value.toString()->ensureLinear(cx);
      if (!currencySign) {
        return false;
      }

      if (StringEqualsLiteral(currencySign, "accounting")) {
        accountingSign = true;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(currencySign, "standard"));
      }

      options.mCurrency = mozilla::Some(
          std::make_pair(std::string_view(options.currencyChars, 3), display));
    } else if (StringEqualsLiteral(style, "percent")) {
      options.mPercent = true;
    } else if (StringEqualsLiteral(style, "unit")) {
      if (!GetProperty(cx, internals, internals, cx->names().unit, &value)) {
        return false;
      }
      JSLinearString* unit = value.toString()->ensureLinear(cx);
      if (!unit) {
        return false;
      }

      size_t unit_str_length = unit->length();

      MOZ_ASSERT(StringIsAscii(unit));
      MOZ_RELEASE_ASSERT(unit_str_length <= MaxUnitLength());
      CopyChars(reinterpret_cast<Latin1Char*>(options.unitChars), *unit);

      if (!GetProperty(cx, internals, internals, cx->names().unitDisplay,
                       &value)) {
        return false;
      }
      JSLinearString* unitDisplay = value.toString()->ensureLinear(cx);
      if (!unitDisplay) {
        return false;
      }

      using UnitDisplay = mozilla::intl::NumberFormatOptions::UnitDisplay;

      UnitDisplay display;
      if (StringEqualsLiteral(unitDisplay, "short")) {
        display = UnitDisplay::Short;
      } else if (StringEqualsLiteral(unitDisplay, "narrow")) {
        display = UnitDisplay::Narrow;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(unitDisplay, "long"));
        display = UnitDisplay::Long;
      }

      options.mUnit = mozilla::Some(std::make_pair(
          std::string_view(options.unitChars, unit_str_length), display));
    } else {
      MOZ_ASSERT(StringEqualsLiteral(style, "decimal"));
    }
  }

  bool hasMinimumSignificantDigits;
  if (!HasProperty(cx, internals, cx->names().minimumSignificantDigits,
                   &hasMinimumSignificantDigits)) {
    return false;
  }

  if (hasMinimumSignificantDigits) {
    if (!GetProperty(cx, internals, internals,
                     cx->names().minimumSignificantDigits, &value)) {
      return false;
    }
    uint32_t minimumSignificantDigits = AssertedCast<uint32_t>(value.toInt32());

    if (!GetProperty(cx, internals, internals,
                     cx->names().maximumSignificantDigits, &value)) {
      return false;
    }
    uint32_t maximumSignificantDigits = AssertedCast<uint32_t>(value.toInt32());

    options.mSignificantDigits = mozilla::Some(
        std::make_pair(minimumSignificantDigits, maximumSignificantDigits));
  }

  bool hasMinimumFractionDigits;
  if (!HasProperty(cx, internals, cx->names().minimumFractionDigits,
                   &hasMinimumFractionDigits)) {
    return false;
  }

  if (hasMinimumFractionDigits) {
    if (!GetProperty(cx, internals, internals,
                     cx->names().minimumFractionDigits, &value)) {
      return false;
    }
    uint32_t minimumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

    if (!GetProperty(cx, internals, internals,
                     cx->names().maximumFractionDigits, &value)) {
      return false;
    }
    uint32_t maximumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

    options.mFractionDigits = mozilla::Some(
        std::make_pair(minimumFractionDigits, maximumFractionDigits));
  }

  if (!GetProperty(cx, internals, internals, cx->names().roundingPriority,
                   &value)) {
    return false;
  }

  {
    JSLinearString* roundingPriority = value.toString()->ensureLinear(cx);
    if (!roundingPriority) {
      return false;
    }

    using RoundingPriority =
        mozilla::intl::NumberFormatOptions::RoundingPriority;

    RoundingPriority priority;
    if (StringEqualsLiteral(roundingPriority, "auto")) {
      priority = RoundingPriority::Auto;
    } else if (StringEqualsLiteral(roundingPriority, "morePrecision")) {
      priority = RoundingPriority::MorePrecision;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(roundingPriority, "lessPrecision"));
      priority = RoundingPriority::LessPrecision;
    }

    options.mRoundingPriority = priority;
  }

  if (!GetProperty(cx, internals, internals, cx->names().minimumIntegerDigits,
                   &value)) {
    return false;
  }
  options.mMinIntegerDigits =
      mozilla::Some(AssertedCast<uint32_t>(value.toInt32()));

  if (!GetProperty(cx, internals, internals, cx->names().useGrouping, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* useGrouping = value.toString()->ensureLinear(cx);
    if (!useGrouping) {
      return false;
    }

    using Grouping = mozilla::intl::NumberFormatOptions::Grouping;

    Grouping grouping;
    if (StringEqualsLiteral(useGrouping, "auto")) {
      grouping = Grouping::Auto;
    } else if (StringEqualsLiteral(useGrouping, "always")) {
      grouping = Grouping::Always;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(useGrouping, "min2"));
      grouping = Grouping::Min2;
    }

    options.mGrouping = grouping;
  } else {
    MOZ_ASSERT(value.isBoolean());
#ifdef NIGHTLY_BUILD
    // The caller passes the string "always" instead of |true| when the
    // NumberFormat V3 spec is being used.
    MOZ_ASSERT(value.toBoolean() == false);
#endif

    using Grouping = mozilla::intl::NumberFormatOptions::Grouping;

    Grouping grouping;
    if (value.toBoolean()) {
      grouping = Grouping::Auto;
    } else {
      grouping = Grouping::Never;
    }

    options.mGrouping = grouping;
  }

  if (!GetProperty(cx, internals, internals, cx->names().notation, &value)) {
    return false;
  }

  {
    JSLinearString* notation = value.toString()->ensureLinear(cx);
    if (!notation) {
      return false;
    }

    using Notation = mozilla::intl::NumberFormatOptions::Notation;

    Notation style;
    if (StringEqualsLiteral(notation, "standard")) {
      style = Notation::Standard;
    } else if (StringEqualsLiteral(notation, "scientific")) {
      style = Notation::Scientific;
    } else if (StringEqualsLiteral(notation, "engineering")) {
      style = Notation::Engineering;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(notation, "compact"));

      if (!GetProperty(cx, internals, internals, cx->names().compactDisplay,
                       &value)) {
        return false;
      }

      JSLinearString* compactDisplay = value.toString()->ensureLinear(cx);
      if (!compactDisplay) {
        return false;
      }

      if (StringEqualsLiteral(compactDisplay, "short")) {
        style = Notation::CompactShort;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(compactDisplay, "long"));
        style = Notation::CompactLong;
      }
    }

    options.mNotation = style;
  }

  if (!GetProperty(cx, internals, internals, cx->names().signDisplay, &value)) {
    return false;
  }

  {
    JSLinearString* signDisplay = value.toString()->ensureLinear(cx);
    if (!signDisplay) {
      return false;
    }

    using SignDisplay = mozilla::intl::NumberFormatOptions::SignDisplay;

    SignDisplay display;
    if (StringEqualsLiteral(signDisplay, "auto")) {
      if (accountingSign) {
        display = SignDisplay::Accounting;
      } else {
        display = SignDisplay::Auto;
      }
    } else if (StringEqualsLiteral(signDisplay, "never")) {
      display = SignDisplay::Never;
    } else if (StringEqualsLiteral(signDisplay, "always")) {
      if (accountingSign) {
        display = SignDisplay::AccountingAlways;
      } else {
        display = SignDisplay::Always;
      }
    } else if (StringEqualsLiteral(signDisplay, "exceptZero")) {
      if (accountingSign) {
        display = SignDisplay::AccountingExceptZero;
      } else {
        display = SignDisplay::ExceptZero;
      }
    } else {
      MOZ_ASSERT(StringEqualsLiteral(signDisplay, "negative"));
      if (accountingSign) {
        display = SignDisplay::AccountingNegative;
      } else {
        display = SignDisplay::Negative;
      }
    }

    options.mSignDisplay = display;
  }

  if (!GetProperty(cx, internals, internals, cx->names().roundingIncrement,
                   &value)) {
    return false;
  }
  options.mRoundingIncrement = AssertedCast<uint32_t>(value.toInt32());

  if (!GetProperty(cx, internals, internals, cx->names().roundingMode,
                   &value)) {
    return false;
  }

  {
    JSLinearString* roundingMode = value.toString()->ensureLinear(cx);
    if (!roundingMode) {
      return false;
    }

    using RoundingMode = mozilla::intl::NumberFormatOptions::RoundingMode;

    RoundingMode rounding;
    if (StringEqualsLiteral(roundingMode, "halfExpand")) {
      // "halfExpand" is the default mode, so we handle it first.
      rounding = RoundingMode::HalfExpand;
    } else if (StringEqualsLiteral(roundingMode, "ceil")) {
      rounding = RoundingMode::Ceil;
    } else if (StringEqualsLiteral(roundingMode, "floor")) {
      rounding = RoundingMode::Floor;
    } else if (StringEqualsLiteral(roundingMode, "expand")) {
      rounding = RoundingMode::Expand;
    } else if (StringEqualsLiteral(roundingMode, "trunc")) {
      rounding = RoundingMode::Trunc;
    } else if (StringEqualsLiteral(roundingMode, "halfCeil")) {
      rounding = RoundingMode::HalfCeil;
    } else if (StringEqualsLiteral(roundingMode, "halfFloor")) {
      rounding = RoundingMode::HalfFloor;
    } else if (StringEqualsLiteral(roundingMode, "halfTrunc")) {
      rounding = RoundingMode::HalfTrunc;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(roundingMode, "halfEven"));
      rounding = RoundingMode::HalfEven;
    }

    options.mRoundingMode = rounding;
  }

  if (!GetProperty(cx, internals, internals, cx->names().trailingZeroDisplay,
                   &value)) {
    return false;
  }

  {
    JSLinearString* trailingZeroDisplay = value.toString()->ensureLinear(cx);
    if (!trailingZeroDisplay) {
      return false;
    }

    if (StringEqualsLiteral(trailingZeroDisplay, "auto")) {
      options.mStripTrailingZero = false;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(trailingZeroDisplay, "stripIfInteger"));
      options.mStripTrailingZero = true;
    }
  }

  return true;
}

/**
 * Returns a new mozilla::intl::Number[Range]Format with the locale and number
 * formatting options of the given NumberFormat, or a nullptr if
 * initialization failed.
 */
template <class Formatter>
static Formatter* NewNumberFormat(JSContext* cx,
                                  Handle<NumberFormatObject*> numberFormat) {
  RootedObject internals(cx, intl::GetInternalsObject(cx, numberFormat));
  if (!internals) {
    return nullptr;
  }

  UniqueChars locale = NumberFormatLocale(cx, internals);
  if (!locale) {
    return nullptr;
  }

  NumberFormatOptions options;
  if (!FillNumberFormatOptions(cx, internals, options)) {
    return nullptr;
  }

  options.mRangeCollapse = NumberFormatOptions::RangeCollapse::Auto;
  options.mRangeIdentityFallback =
      NumberFormatOptions::RangeIdentityFallback::Approximately;

  mozilla::Result<mozilla::UniquePtr<Formatter>,
                  typename Formatter::FormatError>
      result = Formatter::TryCreate(locale.get(), options);

  if (result.isOk()) {
    return result.unwrap().release();
  }

  intl::ReportInternalError(cx);
  return nullptr;
}

static JSString* FormattedNumberToString(
    JSContext* cx, const UFormattedValue* formattedValue) {
  UErrorCode status = U_ZERO_ERROR;
  int32_t strLength;
  const char16_t* str = ufmtval_getString(formattedValue, &strLength, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return nullptr;
  }

  return NewStringCopyN<CanGC>(cx, str, AssertedCast<uint32_t>(strLength));
}

enum class FormattingType { ForUnit, NotForUnit };

static FieldType GetFieldTypeForNumberField(UNumberFormatFields fieldName,
                                            HandleValue x,
                                            FormattingType formattingType) {
  // See intl/icu/source/i18n/unicode/unum.h for a detailed field list.  This
  // list is deliberately exhaustive: cases might have to be added/removed if
  // this code is compiled with a different ICU with more UNumberFormatFields
  // enum initializers.  Please guard such cases with appropriate ICU
  // version-testing #ifdefs, should cross-version divergence occur.
  switch (fieldName) {
    case UNUM_INTEGER_FIELD:
      if (x.isNumber()) {
        double d = x.toNumber();
        if (IsNaN(d)) {
          return &JSAtomState::nan;
        }
        if (!IsFinite(d)) {
          return &JSAtomState::infinity;
        }
      }
      return &JSAtomState::integer;

    case UNUM_GROUPING_SEPARATOR_FIELD:
      return &JSAtomState::group;

    case UNUM_DECIMAL_SEPARATOR_FIELD:
      return &JSAtomState::decimal;

    case UNUM_FRACTION_FIELD:
      return &JSAtomState::fraction;

    case UNUM_SIGN_FIELD: {
      // We coerce all NaNs to one with the sign bit unset, so all NaNs are
      // positive in our implementation.
      bool isNegative = x.isNumber()
                            ? !IsNaN(x.toNumber()) && IsNegative(x.toNumber())
                            : x.toBigInt()->isNegative();
      return isNegative ? &JSAtomState::minusSign : &JSAtomState::plusSign;
    }

    case UNUM_PERCENT_FIELD:
      // Percent fields are returned as "unit" elements when the number
      // formatter's style is "unit".
      if (formattingType == FormattingType::ForUnit) {
        return &JSAtomState::unit;
      }
      return &JSAtomState::percentSign;

    case UNUM_CURRENCY_FIELD:
      return &JSAtomState::currency;

    case UNUM_PERMILL_FIELD:
      MOZ_ASSERT_UNREACHABLE(
          "unexpected permill field found, even though "
          "we don't use any user-defined patterns that "
          "would require a permill field");
      break;

    case UNUM_EXPONENT_SYMBOL_FIELD:
      return &JSAtomState::exponentSeparator;

    case UNUM_EXPONENT_SIGN_FIELD:
      return &JSAtomState::exponentMinusSign;

    case UNUM_EXPONENT_FIELD:
      return &JSAtomState::exponentInteger;

    case UNUM_MEASURE_UNIT_FIELD:
      return &JSAtomState::unit;

    case UNUM_COMPACT_FIELD:
      return &JSAtomState::compact;

#ifndef U_HIDE_DEPRECATED_API
    case UNUM_FIELD_COUNT:
      MOZ_ASSERT_UNREACHABLE(
          "format field sentinel value returned by iterator!");
      break;
#endif
  }

  MOZ_ASSERT_UNREACHABLE(
      "unenumerated, undocumented format field returned by iterator");
  return nullptr;
}

struct Field {
  uint32_t begin;
  uint32_t end;
  FieldType type;

  // Needed for vector-resizing scratch space.
  Field() = default;

  Field(uint32_t begin, uint32_t end, FieldType type)
      : begin(begin), end(end), type(type) {}
};

class NumberFormatFields {
  using FieldsVector = Vector<Field, 16>;

  FieldsVector fields_;

 public:
  explicit NumberFormatFields(JSContext* cx) : fields_(cx) {}

  [[nodiscard]] bool append(FieldType type, int32_t begin, int32_t end);

  [[nodiscard]] ArrayObject* toArray(JSContext* cx,
                                     JS::HandleString overallResult,
                                     FieldType unitType);
};

bool NumberFormatFields::append(FieldType type, int32_t begin, int32_t end) {
  MOZ_ASSERT(begin >= 0);
  MOZ_ASSERT(end >= 0);
  MOZ_ASSERT(begin < end, "erm, aren't fields always non-empty?");

  return fields_.emplaceBack(uint32_t(begin), uint32_t(end), type);
}

ArrayObject* NumberFormatFields::toArray(JSContext* cx,
                                         HandleString overallResult,
                                         FieldType unitType) {
  // Merge sort the fields vector.  Expand the vector to have scratch space for
  // performing the sort.
  size_t fieldsLen = fields_.length();
  if (!fields_.growByUninitialized(fieldsLen)) {
    return nullptr;
  }

  MOZ_ALWAYS_TRUE(MergeSort(
      fields_.begin(), fieldsLen, fields_.begin() + fieldsLen,
      [](const Field& left, const Field& right, bool* lessOrEqual) {
        // Sort first by begin index, then to place
        // enclosing fields before nested fields.
        *lessOrEqual = left.begin < right.begin ||
                       (left.begin == right.begin && left.end > right.end);
        return true;
      }));

  // Delete the elements in the scratch space.
  fields_.shrinkBy(fieldsLen);

  // Then iterate over the sorted field list to generate a sequence of parts
  // (what ECMA-402 actually exposes).  A part is a maximal character sequence
  // entirely within no field or a single most-nested field.
  //
  // Diagrams may be helpful to illustrate how fields map to parts.  Consider
  // formatting -19,766,580,028,249.41, the US national surplus (negative
  // because it's actually a debt) on October 18, 2016.
  //
  //    var options =
  //      { style: "currency", currency: "USD", currencyDisplay: "name" };
  //    var usdFormatter = new Intl.NumberFormat("en-US", options);
  //    usdFormatter.format(-19766580028249.41);
  //
  // The formatted result is "-19,766,580,028,249.41 US dollars".  ICU
  // identifies these fields in the string:
  //
  //     UNUM_GROUPING_SEPARATOR_FIELD
  //                   |
  //   UNUM_SIGN_FIELD |  UNUM_DECIMAL_SEPARATOR_FIELD
  //    |   __________/|   |
  //    |  /   |   |   |   |
  //   "-19,766,580,028,249.41 US dollars"
  //     \________________/ |/ \_______/
  //             |          |      |
  //    UNUM_INTEGER_FIELD  |  UNUM_CURRENCY_FIELD
  //                        |
  //               UNUM_FRACTION_FIELD
  //
  // These fields map to parts as follows:
  //
  //         integer     decimal
  //       _____|________  |
  //      /  /| |\  |\  |\ |  literal
  //     /| / | | \ | \ | \|  |
  //   "-19,766,580,028,249.41 US dollars"
  //    |  \___|___|___/    |/ \________/
  //    |        |          |       |
  //    |      group        |   currency
  //    |                   |
  //   minusSign        fraction
  //
  // The sign is a part.  Each comma is a part, splitting the integer field
  // into parts for trillions/billions/&c. digits.  The decimal point is a
  // part.  Cents are a part.  The space between cents and currency is a part
  // (outside any field).  Last, the currency field is a part.
  //
  // Because parts fully partition the formatted string, we only track the
  // end of each part -- the beginning is implicitly the last part's end.
  struct Part {
    uint32_t end;
    FieldType type;
  };

  class PartGenerator {
    // The fields in order from start to end, then least to most nested.
    const FieldsVector& fields;

    // Index of the current field, in |fields|, being considered to
    // determine part boundaries.  |lastEnd <= fields[index].begin| is an
    // invariant.
    size_t index;

    // The end index of the last part produced, always less than or equal
    // to |limit|, strictly increasing.
    uint32_t lastEnd;

    // The length of the overall formatted string.
    const uint32_t limit;

    Vector<size_t, 4> enclosingFields;

    void popEnclosingFieldsEndingAt(uint32_t end) {
      MOZ_ASSERT_IF(enclosingFields.length() > 0,
                    fields[enclosingFields.back()].end >= end);

      while (enclosingFields.length() > 0 &&
             fields[enclosingFields.back()].end == end) {
        enclosingFields.popBack();
      }
    }

    bool nextPartInternal(Part* part) {
      size_t len = fields.length();
      MOZ_ASSERT(index <= len);

      // If we're out of fields, all that remains are part(s) consisting
      // of trailing portions of enclosing fields, and maybe a final
      // literal part.
      if (index == len) {
        if (enclosingFields.length() > 0) {
          const auto& enclosing = fields[enclosingFields.popCopy()];
          part->end = enclosing.end;
          part->type = enclosing.type;

          // If additional enclosing fields end where this part ends,
          // pop them as well.
          popEnclosingFieldsEndingAt(part->end);
        } else {
          part->end = limit;
          part->type = &JSAtomState::literal;
        }

        return true;
      }

      // Otherwise we still have a field to process.
      const Field* current = &fields[index];
      MOZ_ASSERT(lastEnd <= current->begin);
      MOZ_ASSERT(current->begin < current->end);

      // But first, deal with inter-field space.
      if (lastEnd < current->begin) {
        if (enclosingFields.length() > 0) {
          // Space between fields, within an enclosing field, is part
          // of that enclosing field, until the start of the current
          // field or the end of the enclosing field, whichever is
          // earlier.
          const auto& enclosing = fields[enclosingFields.back()];
          part->end = std::min(enclosing.end, current->begin);
          part->type = enclosing.type;
          popEnclosingFieldsEndingAt(part->end);
        } else {
          // If there's no enclosing field, the space is a literal.
          part->end = current->begin;
          part->type = &JSAtomState::literal;
        }

        return true;
      }

      // Otherwise, the part spans a prefix of the current field.  Find
      // the most-nested field containing that prefix.
      const Field* next;
      do {
        current = &fields[index];

        // If the current field is last, the part extends to its end.
        if (++index == len) {
          part->end = current->end;
          part->type = current->type;
          return true;
        }

        next = &fields[index];
        MOZ_ASSERT(current->begin <= next->begin);
        MOZ_ASSERT(current->begin < next->end);

        // If the next field nests within the current field, push an
        // enclosing field.  (If there are no nested fields, don't
        // bother pushing a field that'd be immediately popped.)
        if (current->end > next->begin) {
          if (!enclosingFields.append(index - 1)) {
            return false;
          }
        }

        // Do so until the next field begins after this one.
      } while (current->begin == next->begin);

      part->type = current->type;

      if (current->end <= next->begin) {
        // The next field begins after the current field ends.  Therefore
        // the current part ends at the end of the current field.
        part->end = current->end;
        popEnclosingFieldsEndingAt(part->end);
      } else {
        // The current field encloses the next one.  The current part
        // ends where the next field/part will start.
        part->end = next->begin;
      }

      return true;
    }

   public:
    PartGenerator(JSContext* cx, const FieldsVector& vec, uint32_t limit)
        : fields(vec),
          index(0),
          lastEnd(0),
          limit(limit),
          enclosingFields(cx) {}

    bool nextPart(bool* hasPart, Part* part) {
      // There are no parts left if we've partitioned the entire string.
      if (lastEnd == limit) {
        MOZ_ASSERT(enclosingFields.length() == 0);
        *hasPart = false;
        return true;
      }

      if (!nextPartInternal(part)) {
        return false;
      }

      *hasPart = true;
      lastEnd = part->end;
      return true;
    }
  };

  // Finally, generate the result array.
  size_t lastEndIndex = 0;
  RootedObject singlePart(cx);
  RootedValue propVal(cx);

  RootedArrayObject partsArray(cx, NewDenseEmptyArray(cx));
  if (!partsArray) {
    return nullptr;
  }

  PartGenerator gen(cx, fields_, overallResult->length());
  do {
    bool hasPart = false;
    Part part = {};
    if (!gen.nextPart(&hasPart, &part)) {
      return nullptr;
    }

    if (!hasPart) {
      break;
    }

    FieldType type = part.type;
    size_t endIndex = part.end;

    MOZ_ASSERT(lastEndIndex < endIndex);

    singlePart = NewBuiltinClassInstance<PlainObject>(cx);
    if (!singlePart) {
      return nullptr;
    }

    propVal.setString(cx->names().*type);
    if (!DefineDataProperty(cx, singlePart, cx->names().type, propVal)) {
      return nullptr;
    }

    JSLinearString* partSubstr = NewDependentString(
        cx, overallResult, lastEndIndex, endIndex - lastEndIndex);
    if (!partSubstr) {
      return nullptr;
    }

    propVal.setString(partSubstr);
    if (!DefineDataProperty(cx, singlePart, cx->names().value, propVal)) {
      return nullptr;
    }

    if (unitType != nullptr && type != &JSAtomState::literal) {
      propVal.setString(cx->names().*unitType);
      if (!DefineDataProperty(cx, singlePart, cx->names().unit, propVal)) {
        return nullptr;
      }
    }

    if (!NewbornArrayPush(cx, partsArray, ObjectValue(*singlePart))) {
      return nullptr;
    }

    lastEndIndex = endIndex;
  } while (true);

  MOZ_ASSERT(lastEndIndex == overallResult->length(),
             "result array must partition the entire string");

  return partsArray;
}

static bool FormattedNumberToParts(JSContext* cx,
                                   const UFormattedValue* formattedValue,
                                   HandleValue number,
                                   FieldType relativeTimeUnit,
                                   FormattingType formattingType,
                                   MutableHandleValue result) {
  MOZ_ASSERT(number.isNumeric());

  RootedString overallResult(cx, FormattedNumberToString(cx, formattedValue));
  if (!overallResult) {
    return false;
  }

  UErrorCode status = U_ZERO_ERROR;
  UConstrainedFieldPosition* fpos = ucfpos_open(&status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UConstrainedFieldPosition, ucfpos_close> toCloseFpos(fpos);

  // We're only interested in UFIELD_CATEGORY_NUMBER fields.
  ucfpos_constrainCategory(fpos, UFIELD_CATEGORY_NUMBER, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  // Vacuum up fields in the overall formatted string.

  NumberFormatFields fields(cx);

  while (true) {
    bool hasMore = ufmtval_nextPosition(formattedValue, fpos, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }
    if (!hasMore) {
      break;
    }

    int32_t field = ucfpos_getField(fpos, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }

    int32_t beginIndex, endIndex;
    ucfpos_getIndexes(fpos, &beginIndex, &endIndex, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }

    FieldType type = GetFieldTypeForNumberField(UNumberFormatFields(field),
                                                number, formattingType);

    if (!fields.append(type, beginIndex, endIndex)) {
      return false;
    }
  }

  ArrayObject* array = fields.toArray(cx, overallResult, relativeTimeUnit);
  if (!array) {
    return false;
  }

  result.setObject(*array);
  return true;
}

bool js::intl::FormattedRelativeTimeToParts(
    JSContext* cx, const UFormattedValue* formattedValue, double timeValue,
    FieldType relativeTimeUnit, MutableHandleValue result) {
  Value tval = DoubleValue(timeValue);
  return FormattedNumberToParts(
      cx, formattedValue, HandleValue::fromMarkedLocation(&tval),
      relativeTimeUnit, FormattingType::NotForUnit, result);
}

static FieldType GetFieldTypeForNumberPartType(
    mozilla::intl::NumberPartType type) {
  switch (type) {
    case mozilla::intl::NumberPartType::ApproximatelySign:
      return &JSAtomState::approximatelySign;
    case mozilla::intl::NumberPartType::Compact:
      return &JSAtomState::compact;
    case mozilla::intl::NumberPartType::Currency:
      return &JSAtomState::currency;
    case mozilla::intl::NumberPartType::Decimal:
      return &JSAtomState::decimal;
    case mozilla::intl::NumberPartType::ExponentInteger:
      return &JSAtomState::exponentInteger;
    case mozilla::intl::NumberPartType::ExponentMinusSign:
      return &JSAtomState::exponentMinusSign;
    case mozilla::intl::NumberPartType::ExponentSeparator:
      return &JSAtomState::exponentSeparator;
    case mozilla::intl::NumberPartType::Fraction:
      return &JSAtomState::fraction;
    case mozilla::intl::NumberPartType::Group:
      return &JSAtomState::group;
    case mozilla::intl::NumberPartType::Infinity:
      return &JSAtomState::infinity;
    case mozilla::intl::NumberPartType::Integer:
      return &JSAtomState::integer;
    case mozilla::intl::NumberPartType::Literal:
      return &JSAtomState::literal;
    case mozilla::intl::NumberPartType::MinusSign:
      return &JSAtomState::minusSign;
    case mozilla::intl::NumberPartType::Nan:
      return &JSAtomState::nan;
    case mozilla::intl::NumberPartType::Percent:
      return &JSAtomState::percentSign;
    case mozilla::intl::NumberPartType::PlusSign:
      return &JSAtomState::plusSign;
    case mozilla::intl::NumberPartType::Unit:
      return &JSAtomState::unit;
  }

  MOZ_ASSERT_UNREACHABLE(
      "unenumerated, undocumented format field returned by iterator");
  return nullptr;
}

static FieldType GetFieldTypeForNumberPartSource(
    mozilla::intl::NumberPartSource source) {
  switch (source) {
    case mozilla::intl::NumberPartSource::Shared:
      return &JSAtomState::shared;
    case mozilla::intl::NumberPartSource::Start:
      return &JSAtomState::startRange;
    case mozilla::intl::NumberPartSource::End:
      return &JSAtomState::endRange;
  }

  MOZ_CRASH("unexpected number part source");
}

enum class DisplayNumberPartSource : bool { No, Yes };

static bool FormattedNumberToParts(JSContext* cx, HandleString str,
                                   const mozilla::intl::NumberPartVector& parts,
                                   DisplayNumberPartSource displaySource,
                                   MutableHandleValue result) {
  size_t lastEndIndex = 0;

  RootedObject singlePart(cx);
  RootedValue propVal(cx);

  RootedArrayObject partsArray(cx,
                               NewDenseFullyAllocatedArray(cx, parts.length()));
  if (!partsArray) {
    return false;
  }
  partsArray->ensureDenseInitializedLength(0, parts.length());

  size_t index = 0;
  for (const auto& part : parts) {
    FieldType type = GetFieldTypeForNumberPartType(part.type);
    size_t endIndex = part.endIndex;

    MOZ_ASSERT(lastEndIndex < endIndex);

    singlePart = NewBuiltinClassInstance<PlainObject>(cx);
    if (!singlePart) {
      return false;
    }

    propVal.setString(cx->names().*type);
    if (!DefineDataProperty(cx, singlePart, cx->names().type, propVal)) {
      return false;
    }

    JSLinearString* partSubstr =
        NewDependentString(cx, str, lastEndIndex, endIndex - lastEndIndex);
    if (!partSubstr) {
      return false;
    }

    propVal.setString(partSubstr);
    if (!DefineDataProperty(cx, singlePart, cx->names().value, propVal)) {
      return false;
    }

    if (displaySource == DisplayNumberPartSource::Yes) {
      FieldType source = GetFieldTypeForNumberPartSource(part.source);

      propVal.setString(cx->names().*source);
      if (!DefineDataProperty(cx, singlePart, cx->names().source, propVal)) {
        return false;
      }
    }

    partsArray->initDenseElement(index++, ObjectValue(*singlePart));

    lastEndIndex = endIndex;
  }

  MOZ_ASSERT(index == parts.length());
  MOZ_ASSERT(lastEndIndex == str->length(),
             "result array must partition the entire string");

  result.setObject(*partsArray);
  return true;
}

// Return true if the string starts with "0[bBoOxX]", possibly skipping over
// leading whitespace.
template <typename CharT>
static bool IsNonDecimalNumber(mozilla::Range<const CharT> chars) {
  const CharT* end = chars.begin().get() + chars.length();
  const CharT* start = SkipSpace(chars.begin().get(), end);

  if (end - start >= 2 && start[0] == '0') {
    CharT ch = start[1];
    return ch == 'b' || ch == 'B' || ch == 'o' || ch == 'O' || ch == 'x' ||
           ch == 'X';
  }
  return false;
}

static bool IsNonDecimalNumber(JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  return str->hasLatin1Chars() ? IsNonDecimalNumber(str->latin1Range(nogc))
                               : IsNonDecimalNumber(str->twoByteRange(nogc));
}

static bool ToIntlMathematicalValue(JSContext* cx, MutableHandleValue value,
                                    double* numberApproximation = nullptr) {
  if (!ToPrimitive(cx, JSTYPE_NUMBER, value)) {
    return false;
  }

  // Maximum exponent supported by ICU. Exponents larger than this value will
  // cause ICU to report an error.
  // See also "intl/icu/source/i18n/decContext.h".
  constexpr int32_t maximumExponent = 999'999'999;

  // When formatting a number range, ICU inserts the integer digits of the
  // second number into the middle of the result string. This leads to calling
  // memmove for each digit, which causes tremendous slowdowns. Therefore we
  // additionally limit the maximum positive exponent.
  //
  // Filed at <https://unicode-org.atlassian.net/browse/ICU-21684>.
  constexpr int32_t maximumPositiveExponent = 99'999;

  // Compute the maximum BigInt digit length from the maximum positive exponent.
  //
  // BigInts are stored with base |2 ** BigInt::DigitBits|, so we have:
  //
  //   |maximumPositiveExponent| * Log_DigitBase(10)
  // = |maximumPositiveExponent| * Log2(10) / Log2(2 ** BigInt::DigitBits)
  // = |maximumPositiveExponent| * Log2(10) / BigInt::DigitBits
  // = 332189.4875606413... / BigInt::DigitBits
  constexpr size_t maximumBigIntLength = 332189.4875606413 / BigInt::DigitBits;

  if (!value.isString()) {
    if (!ToNumeric(cx, value)) {
      return false;
    }

    if (value.isBigInt() &&
        value.toBigInt()->digitLength() > maximumBigIntLength) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_EXPONENT_TOO_LARGE);
      return false;
    }

    return true;
  }

  JSLinearString* str = value.toString()->ensureLinear(cx);
  if (!str) {
    return false;
  }

  // Call StringToNumber to validate the input can be parsed as a number.
  double number;
  if (!StringToNumber(cx, str, &number)) {
    return false;
  }
  if (numberApproximation) {
    *numberApproximation = number;
  }

  bool exponentTooLarge = false;
  if (mozilla::IsNaN(number)) {
    // Set to NaN if the input can't be parsed as a number.
    value.setNaN();
  } else if (IsNonDecimalNumber(str)) {
    // ICU doesn't accept non-decimal numbers, so we have to convert the input
    // into a base-10 string.

    MOZ_ASSERT(!mozilla::IsNegative(number),
               "non-decimal numbers can't be negative");

    if (number < DOUBLE_INTEGRAL_PRECISION_LIMIT) {
      // Fast-path if we can guarantee there was no loss of precision.
      value.setDouble(number);
    } else {
      // For the slow-path convert the string into a BigInt.

      // StringToBigInt can't fail (other than OOM) when StringToNumber already
      // succeeded.
      RootedString rooted(cx, str);
      BigInt* bi;
      JS_TRY_VAR_OR_RETURN_FALSE(cx, bi, StringToBigInt(cx, rooted));
      MOZ_ASSERT(bi);

      if (bi->digitLength() > maximumBigIntLength) {
        exponentTooLarge = true;
      } else {
        value.setBigInt(bi);
      }
    }
  } else {
    JS::AutoCheckCannotGC nogc;
    if (auto decimal = intl::DecimalNumber::from(str, nogc)) {
      if (decimal->isZero()) {
        // Normalize positive/negative zero.
        MOZ_ASSERT(number == 0);

        value.setDouble(number);
      } else if (decimal->exponentTooLarge() ||
                 std::abs(decimal->exponent()) >= maximumExponent ||
                 decimal->exponent() > maximumPositiveExponent) {
        exponentTooLarge = true;
      }
    } else {
      // If we can't parse the string as a decimal, it must be ±Infinity.
      MOZ_ASSERT(mozilla::IsInfinite(number));
      MOZ_ASSERT(StringFindPattern(str, cx->names().Infinity, 0) >= 0);

      value.setDouble(number);
    }
  }

  if (exponentTooLarge) {
    // Throw an error if the exponent is too large.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_EXPONENT_TOO_LARGE);
    return false;
  }

  return true;
}

// Return the number part of the input by removing leading and trailing
// whitespace.
template <typename CharT>
static mozilla::Span<const CharT> NumberPart(const CharT* chars,
                                             size_t length) {
  const CharT* start = chars;
  const CharT* end = chars + length;

  start = SkipSpace(start, end);

  // |SkipSpace| only supports forward iteration, so inline the backwards
  // iteration here.
  MOZ_ASSERT(start <= end);
  while (end > start && unicode::IsSpace(end[-1])) {
    end--;
  }

  // The number part is a non-empty, ASCII-only substring.
  MOZ_ASSERT(start < end);
  MOZ_ASSERT(mozilla::IsAscii(mozilla::Span(start, end)));

  return {start, end};
}

static bool NumberPart(JSContext* cx, JSLinearString* str,
                       const JS::AutoCheckCannotGC& nogc,
                       JS::UniqueChars& latin1, std::string_view& result) {
  if (str->hasLatin1Chars()) {
    auto span = NumberPart(
        reinterpret_cast<const char*>(str->latin1Chars(nogc)), str->length());

    result = {span.data(), span.size()};
    return true;
  }

  auto span = NumberPart(str->twoByteChars(nogc), str->length());

  latin1.reset(JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, span).c_str());
  if (!latin1) {
    return false;
  }

  result = {latin1.get(), span.size()};
  return true;
}

bool js::intl_FormatNumber(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
#ifndef NIGHTLY_BUILD
  MOZ_ASSERT(args[1].isNumeric());
#endif
  MOZ_ASSERT(args[2].isBoolean());

  Rooted<NumberFormatObject*> numberFormat(
      cx, &args[0].toObject().as<NumberFormatObject>());

  RootedValue value(cx, args[1]);
#ifdef NIGHTLY_BUILD
  if (!ToIntlMathematicalValue(cx, &value)) {
    return false;
  }
#endif

  // Obtain a cached mozilla::intl::NumberFormat object.
  mozilla::intl::NumberFormat* nf = numberFormat->getNumberFormatter();
  if (!nf) {
    nf = NewNumberFormat<mozilla::intl::NumberFormat>(cx, numberFormat);
    if (!nf) {
      return false;
    }
    numberFormat->setNumberFormatter(nf);

    intl::AddICUCellMemory(numberFormat,
                           NumberFormatObject::EstimatedMemoryUse);
  }

  // Actually format the number
  using FormatError = mozilla::intl::NumberFormat::FormatError;

  bool formatToParts = args[2].toBoolean();
  mozilla::Result<std::u16string_view, FormatError> result =
      mozilla::Err(FormatError::InternalError);
  mozilla::intl::NumberPartVector parts;
  if (value.isNumber()) {
    double num = value.toNumber();
    if (formatToParts) {
      result = nf->formatToParts(num, parts);
    } else {
      result = nf->format(num);
    }
  } else if (value.isBigInt()) {
    RootedBigInt bi(cx, value.toBigInt());

    int64_t num;
    if (BigInt::isInt64(bi, &num)) {
      if (formatToParts) {
        result = nf->formatToParts(num, parts);
      } else {
        result = nf->format(num);
      }
    } else {
      JSLinearString* str = BigInt::toString<CanGC>(cx, bi, 10);
      if (!str) {
        return false;
      }
      MOZ_RELEASE_ASSERT(str->hasLatin1Chars());

      JS::AutoCheckCannotGC nogc;

      const char* chars = reinterpret_cast<const char*>(str->latin1Chars(nogc));
      if (formatToParts) {
        result =
            nf->formatToParts(std::string_view(chars, str->length()), parts);
      } else {
        result = nf->format(std::string_view(chars, str->length()));
      }
    }
  } else {
    JSLinearString* str = value.toString()->ensureLinear(cx);
    if (!str) {
      return false;
    }

    JS::AutoCheckCannotGC nogc;

    // Two-byte strings have to be copied into a separate |char| buffer.
    JS::UniqueChars latin1;

    std::string_view sv;
    if (!NumberPart(cx, str, nogc, latin1, sv)) {
      return false;
    }

    if (formatToParts) {
      result = nf->formatToParts(sv, parts);
    } else {
      result = nf->format(sv);
    }
  }

  if (result.isErr()) {
    intl::ReportInternalError(cx);
    return false;
  }

  std::u16string_view result_string_view = result.unwrap();
  RootedString str(cx, NewStringCopyN<CanGC>(
                           cx, result_string_view.data(),
                           AssertedCast<uint32_t>(result_string_view.size())));
  if (!str) {
    return false;
  }

  if (formatToParts) {
    return FormattedNumberToParts(cx, str, parts, DisplayNumberPartSource::No,
                                  args.rval());
  }

  args.rval().setString(str);
  return true;
}

#ifdef MOZ_INTL_HAS_NUMBER_RANGE_FORMAT
static JSLinearString* ToLinearString(JSContext* cx, HandleValue val) {
  // Special case to preserve negative zero.
  if (val.isDouble() && mozilla::IsNegativeZero(val.toDouble())) {
    constexpr std::string_view negativeZero = "-0";
    return NewStringCopyN<CanGC>(cx, negativeZero.data(),
                                 negativeZero.length());
  }

  JSString* str = ToString(cx, val);
  return str ? str->ensureLinear(cx) : nullptr;
};

static bool ValidateNumberRange(JSContext* cx, MutableHandleValue start,
                                double startApprox, MutableHandleValue end,
                                double endApprox, bool formatToParts) {
  static auto isSpecificDouble = [](const Value& val, auto fn) {
    return val.isDouble() && fn(val.toDouble());
  };

  static auto isNaN = [](const Value& val) {
    return isSpecificDouble(val, mozilla::IsNaN<double>);
  };

  static auto isPositiveInfinity = [](const Value& val) {
    return isSpecificDouble(
        val, [](double num) { return num > 0 && mozilla::IsInfinite(num); });
  };

  static auto isNegativeInfinity = [](const Value& val) {
    return isSpecificDouble(
        val, [](double num) { return num < 0 && mozilla::IsInfinite(num); });
  };

  static auto isNegativeZero = [](const Value& val) {
    return isSpecificDouble(val, mozilla::IsNegativeZero<double>);
  };

  static auto isMathematicalValue = [](const Value& val) {
    // |ToIntlMathematicalValue()| normalizes non-finite values and negative
    // zero to Double values, so any string is guaranteed to be a mathematical
    // value at this point.
    if (!val.isDouble()) {
      return true;
    }
    double num = val.toDouble();
    return mozilla::IsFinite(num) && !mozilla::IsNegativeZero(num);
  };

  static auto isPositiveOrZero = [](const Value& val, double approx) {
    MOZ_ASSERT(isMathematicalValue(val));

    if (val.isNumber()) {
      return val.toNumber() >= 0;
    }
    if (val.isBigInt()) {
      return !val.toBigInt()->isNegative();
    }
    return approx >= 0;
  };

  auto throwRangeError = [&]() {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_START_AFTER_END_NUMBER,
        "NumberFormat", formatToParts ? "formatRangeToParts" : "formatRange");
    return false;
  };

  // PartitionNumberRangePattern, step 1.
  if (isNaN(start)) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NAN_NUMBER_RANGE, "start",
        formatToParts ? "formatRangeToParts" : "formatRange");
    return false;
  }
  if (isNaN(end)) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NAN_NUMBER_RANGE, "end",
        formatToParts ? "formatRangeToParts" : "formatRange");
    return false;
  }

  // Make sure |start| and |end| can be correctly classified.
  MOZ_ASSERT(isMathematicalValue(start) || isNegativeZero(start) ||
             isNegativeInfinity(start) || isPositiveInfinity(start));
  MOZ_ASSERT(isMathematicalValue(end) || isNegativeZero(end) ||
             isNegativeInfinity(end) || isPositiveInfinity(end));

  // PartitionNumberRangePattern, step 2.
  if (isMathematicalValue(start)) {
    // PartitionNumberRangePattern, step 2.a.
    if (isMathematicalValue(end)) {
      if (!start.isString() && !end.isString()) {
        MOZ_ASSERT(start.isNumeric() && end.isNumeric());

        bool isLessThan;
        if (!LessThan(cx, end, start, &isLessThan)) {
          return false;
        }
        if (isLessThan) {
          return throwRangeError();
        }
      } else {
        // |startApprox| and |endApprox| are only initially computed for string
        // numbers.
        if (start.isNumber()) {
          startApprox = start.toNumber();
        } else if (start.isBigInt()) {
          startApprox = BigInt::numberValue(start.toBigInt());
        }
        if (end.isNumber()) {
          endApprox = end.toNumber();
        } else if (end.isBigInt()) {
          endApprox = BigInt::numberValue(end.toBigInt());
        }

        // If the approximation is smaller, the actual value is definitely
        // smaller, too.
        if (endApprox < startApprox) {
          return throwRangeError();
        }

        // If both approximations are equal to each other, we have to perform
        // more work.
        if (endApprox == startApprox) {
          RootedLinearString strStart(cx, ToLinearString(cx, start));
          if (!strStart) {
            return false;
          }

          RootedLinearString strEnd(cx, ToLinearString(cx, end));
          if (!strEnd) {
            return false;
          }

          bool endLessThanStart;
          {
            JS::AutoCheckCannotGC nogc;

            auto decStart = intl::DecimalNumber::from(strStart, nogc);
            MOZ_ASSERT(decStart);

            auto decEnd = intl::DecimalNumber::from(strEnd, nogc);
            MOZ_ASSERT(decEnd);

            endLessThanStart = decEnd->compareTo(*decStart) < 0;
          }
          if (endLessThanStart) {
            return throwRangeError();
          }

          // If either value is a string, we end up passing both values as
          // strings to the formatter. So let's save the string representation
          // here, because then we don't have to recompute them later on.
          start.setString(strStart);
          end.setString(strEnd);
        }
      }
    }

    // PartitionNumberRangePattern, step 2.b.
    else if (isNegativeInfinity(end)) {
      return throwRangeError();
    }

    // PartitionNumberRangePattern, step 2.c.
    else if (isNegativeZero(end)) {
      if (isPositiveOrZero(start, startApprox)) {
        return throwRangeError();
      }
    }

    // No range restrictions when the end is positive infinity.
    else {
      MOZ_ASSERT(isPositiveInfinity(end));
    }
  }

  // PartitionNumberRangePattern, step 3.
  else if (isPositiveInfinity(start)) {
    // PartitionNumberRangePattern, steps 3.a-c.
    if (!isPositiveInfinity(end)) {
      return throwRangeError();
    }
  }

  // PartitionNumberRangePattern, step 4.
  else if (isNegativeZero(start)) {
    // PartitionNumberRangePattern, step 4.a.
    if (isMathematicalValue(end)) {
      if (!isPositiveOrZero(end, endApprox)) {
        return throwRangeError();
      }
    }

    // PartitionNumberRangePattern, step 4.b.
    else if (isNegativeInfinity(end)) {
      return throwRangeError();
    }

    // No range restrictions when the end is negative zero or positive infinity.
    else {
      MOZ_ASSERT(isNegativeZero(end) || isPositiveInfinity(end));
    }
  }

  // No range restrictions when the start is negative infinity.
  else {
    MOZ_ASSERT(isNegativeInfinity(start));
  }

  return true;
}
#endif

bool js::intl_FormatNumberRange(JSContext* cx, unsigned argc, Value* vp) {
#ifdef MOZ_INTL_HAS_NUMBER_RANGE_FORMAT
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(!args[1].isUndefined());
  MOZ_ASSERT(!args[2].isUndefined());
  MOZ_ASSERT(args[3].isBoolean());

  Rooted<NumberFormatObject*> numberFormat(
      cx, &args[0].toObject().as<NumberFormatObject>());
  bool formatToParts = args[3].toBoolean();

  RootedValue start(cx, args[1]);
  double startApprox = mozilla::UnspecifiedNaN<double>();
  if (!ToIntlMathematicalValue(cx, &start, &startApprox)) {
    return false;
  }

  RootedValue end(cx, args[2]);
  double endApprox = mozilla::UnspecifiedNaN<double>();
  if (!ToIntlMathematicalValue(cx, &end, &endApprox)) {
    return false;
  }

  if (!ValidateNumberRange(cx, &start, startApprox, &end, endApprox,
                           formatToParts)) {
    return false;
  }

  // Obtain a cached mozilla::intl::NumberFormat object.
  using NumberRangeFormat = mozilla::intl::NumberRangeFormat;
  NumberRangeFormat* nf = numberFormat->getNumberRangeFormatter();
  if (!nf) {
    nf = NewNumberFormat<NumberRangeFormat>(cx, numberFormat);
    if (!nf) {
      return false;
    }
    numberFormat->setNumberRangeFormatter(nf);

    intl::AddICUCellMemory(
        numberFormat, NumberFormatObject::EstimatedRangeFormatterMemoryUse);
  }

  auto valueRepresentableAsDouble = [](const Value& val, double* num) {
    if (val.isNumber()) {
      *num = val.toNumber();
      return true;
    }
    if (val.isBigInt()) {
      int64_t i64;
      if (BigInt::isInt64(val.toBigInt(), &i64) &&
          i64 < int64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT) &&
          i64 > -int64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
        *num = double(i64);
        return true;
      }
    }
    return false;
  };

  // Actually format the number range.
  using FormatError = NumberRangeFormat::FormatError;

  mozilla::Result<std::u16string_view, FormatError> result =
      mozilla::Err(FormatError::InternalError);
  mozilla::intl::NumberPartVector parts;

  double numStart, numEnd;
  if (valueRepresentableAsDouble(start, &numStart) &&
      valueRepresentableAsDouble(end, &numEnd)) {
    if (formatToParts) {
      result = nf->formatToParts(numStart, numEnd, parts);
    } else {
      result = nf->format(numStart, numEnd);
    }
  } else {
    RootedLinearString strStart(cx, ToLinearString(cx, start));
    if (!strStart) {
      return false;
    }

    RootedLinearString strEnd(cx, ToLinearString(cx, end));
    if (!strEnd) {
      return false;
    }

    JS::AutoCheckCannotGC nogc;

    // Two-byte strings have to be copied into a separate |char| buffer.
    JS::UniqueChars latin1Start;
    JS::UniqueChars latin1End;

    std::string_view svStart;
    if (!NumberPart(cx, strStart, nogc, latin1Start, svStart)) {
      return false;
    }

    std::string_view svEnd;
    if (!NumberPart(cx, strEnd, nogc, latin1End, svEnd)) {
      return false;
    }

    if (formatToParts) {
      result = nf->formatToParts(svStart, svEnd, parts);
    } else {
      result = nf->format(svStart, svEnd);
    }
  }

  if (result.isErr()) {
    intl::ReportInternalError(cx);
    return false;
  }

  std::u16string_view result_string_view = result.unwrap();
  RootedString str(cx, NewStringCopyN<CanGC>(cx, result_string_view.data(),
                                             result_string_view.size()));
  if (!str) {
    return false;
  }

  if (formatToParts) {
    return FormattedNumberToParts(cx, str, parts, DisplayNumberPartSource::Yes,
                                  args.rval());
  }

  args.rval().setString(str);
  return true;
#else
  MOZ_CRASH("ICU draft API not enabled");
#endif
}
