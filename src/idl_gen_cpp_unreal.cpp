/*
 * Copyright (c) 2026 Golden Hour Games. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "idl_gen_cpp_unreal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "flatbuffers/code_generators.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

namespace flatbuffers {
namespace cpp_unreal {

// Copied from idl_gen_cpp
inline std::string NumToStringCpp(std::string val, BaseType type) {
  switch (type) {
    case BASE_TYPE_INT:
      return (val != "-2147483648") ? val : ("(-2147483647 - 1)");
    case BASE_TYPE_ULONG:
      return (val == "0") ? val : (val + "ULL");
    case BASE_TYPE_LONG:
      if (val == "-9223372036854775808")
        return "(-9223372036854775807LL - 1LL)";
      else
        return (val == "0") ? val : (val + "LL");
    default:
      return val;
  }
}

// Copied from idl_gen_cpp
inline std::string NativeScalarTypeName(const BaseType base_type) {
  if (IsBool(base_type)) return "bool";
  // clang-format off
  static const char* const ctypename[] = {
    #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, ...) \
      #CTYPE,
      FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
    #undef FLATBUFFERS_TD
  };
  // clang-format on
  return ctypename[base_type];
}

enum CppStandard { CPP_STD_X0 = 0, CPP_STD_11, CPP_STD_17 };

// Copied from idl_gen_cpp
inline bool ParseCppStandard(const std::string& cpp_std_arg,
                             CppStandard* result) {
  std::string cpp_std = !cpp_std_arg.empty() ? cpp_std_arg : "C++11";
  std::transform(cpp_std.begin(), cpp_std.end(), cpp_std.begin(), CharToUpper);
  if (cpp_std == "C++0X") {
    *result = CPP_STD_X0;
  } else if (cpp_std == "C++11") {
    *result = CPP_STD_11;
  } else if (cpp_std == "C++17") {
    *result = CPP_STD_17;
  } else {
    return false;
  }
  return true;
}

inline bool CppScopedEnumsEnabled(const IDLOptions& opts) {
  CppStandard cpp_std;
  if (!ParseCppStandard(opts.cpp_std, &cpp_std)) {
    return opts.scoped_enums;
  }
  return opts.scoped_enums || cpp_std >= CPP_STD_17;
}

inline std::string ObjectApiName(const std::string& name,
                                 const StructDef& struct_def,
                                 const IDLOptions& opts) {
  return !struct_def.fixed ? opts.object_prefix + name + opts.object_suffix
                           : name;
}

inline std::string EscapeCppString(const std::string& text) {
  std::string escaped;
  for (const char c : text) {
    if (c == '\\' || c == '"') escaped += '\\';
    escaped += c;
  }
  return escaped;
}

inline std::string Join(const std::vector<std::string>& parts,
                        const std::string& separator) {
  std::string joined;
  for (const std::string& part : parts) {
    if (!joined.empty()) joined += separator;
    joined += part;
  }
  return joined;
}

class CppUnrealGenerator : public BaseGenerator {
 public:
  CppUnrealGenerator(const Parser& parser, const std::string& path,
                     const std::string& file_name,
                     const std::string& unreal_module)
      : BaseGenerator(parser, path, file_name, "", "::", "h"),
        unreal_module_(unreal_module),
        code_("\t"),
        float_const_gen_("std::numeric_limits<double>::",
                         "std::numeric_limits<float>::", "quiet_NaN()",
                         "infinity()") {}

  std::string error_;

  bool generate() override {
    CollectDefinitions();
    ValidateSpecifiersUsage();
    ValidateFixedPointUsage();
    if (marked_enums_.empty() && marked_structs_.empty()) {
      if (!errors_.empty()) {
        JoinErrors();
        return false;
      }
      return true;
    }

    Validate();
    const std::vector<const StructDef*> sorted_structs =
        SortStructsByDependency();
    if (!errors_.empty()) {
      JoinErrors();
      return false;
    }

    code_.Clear();
    for (const EnumDef* enum_def : marked_enums_) {
      GenEnum(*enum_def);
    }
    for (const StructDef* struct_def : sorted_structs) {
      GenStructDecl(*struct_def);
    }
    for (const StructDef* struct_def : sorted_structs) {
      GenStructConversions(*struct_def);
    }
    const std::string body = code_.ToString();

    code_.Clear();
    GenPreamble();
    std::string content = code_.ToString() + body;
    while (content.size() >= 2 && content[content.size() - 1] == '\n' &&
           content[content.size() - 2] == '\n') {
      content.pop_back();
    }

    const std::string file_path =
        GeneratedFileName(path_, file_name_ + "_unreal", parser_.opts);
    return parser_.opts.file_saver->SaveFile(file_path.c_str(), content,
                                             false);
  }

 private:
  static const Value* UnrealAttribute(const Definition& def) {
    return def.attributes.Lookup("unreal_type");
  }

  static bool HasUnrealAttribute(const Definition& def) {
    return UnrealAttribute(def) != nullptr;
  }

  static bool IsBitFlags(const EnumDef& enum_def) {
    return enum_def.attributes.Lookup("bit_flags") != nullptr;
  }

  static const Value* SpecifiersAttribute(const SymbolTable<Value>& attributes) {
    return attributes.Lookup("unreal_specifiers");
  }

  static std::string SpecifierList(const Value& attribute) {
    return attribute.type.base_type == BASE_TYPE_STRING ? attribute.constant
                                                        : "";
  }

  void JoinErrors() {
    error_.clear();
    for (const std::string& error : errors_) {
      if (!error_.empty()) error_ += "\n  ";
      error_ += error;
    }
  }

  static std::string NameOverride(const Definition& def) {
    const Value* attr = UnrealAttribute(def);
    if (attr && attr->type.base_type == BASE_TYPE_STRING) {
      return attr->constant;
    }
    return "";
  }

  static std::string UnrealEnumName(const EnumDef& enum_def) {
    const std::string override_name = NameOverride(enum_def);
    return override_name.empty() ? "E" + enum_def.name : override_name;
  }

  static std::string UnrealStructName(const StructDef& struct_def) {
    const std::string override_name = NameOverride(struct_def);
    return override_name.empty() ? "F" + struct_def.name : override_name;
  }

  static std::string EnumBaseName(const EnumDef& enum_def) {
    const std::string ue_name = UnrealEnumName(enum_def);
    const bool has_prefix =
        ue_name.size() > 1 && ue_name[0] == 'E' &&
        std::isupper(static_cast<unsigned char>(ue_name[1])) != 0;
    return has_prefix ? ue_name.substr(1) : ue_name;
  }

  static std::string ConvertFunctionName(const EnumDef& enum_def) {
    return "Convert" + EnumBaseName(enum_def);
  }

  std::string EnumScriptPath(const EnumDef& enum_def) const {
    return "/Script/" + unreal_module_ + "." + UnrealEnumName(enum_def);
  }

  std::string StructScriptPath(const StructDef& struct_def) const {
    return "/Script/" + unreal_module_ + "." +
           UnrealStructName(struct_def).substr(1);
  }

  static std::string UnrealFieldName(const FieldDef& field) {
    const std::string snake_name = FixedPointFractionBits(field) != 0
                                       ? WithoutRawWord(field.name)
                                       : field.name;
    const std::string name =
        ConvertCase(snake_name, Case::kUpperCamel, Case::kSnake);
    return IsBool(field.value.type.base_type) ? "b" + name : name;
  }

  static std::string WithoutRawWord(const std::string& snake_name) {
    std::string result;
    size_t start = 0;
    while (start <= snake_name.size()) {
      const size_t end =
          std::min(snake_name.find('_', start), snake_name.size());
      const std::string word = snake_name.substr(start, end - start);
      if (word != "raw") {
        if (!result.empty()) result += '_';
        result += word;
      }
      start = end + 1;
    }
    return result.empty() ? snake_name : result;
  }

  static const Value* FixedPointAttribute(const FieldDef& field) {
    return field.attributes.Lookup("fixed_point");
  }

  static int FixedPointFractionBits(const FieldDef& field) {
    const Value* attribute = FixedPointAttribute(field);
    int fraction_bits = 0;
    if (attribute != nullptr && attribute->type.base_type == BASE_TYPE_INT) {
      StringToNumber(attribute->constant.c_str(), &fraction_bits);
    }
    return fraction_bits;
  }

  static std::string FixedPointScale(const int fraction_bits) {
    return NumToString(uint64_t{1} << fraction_bits) + ".0";
  }

  std::string QualifiedNativeName(const StructDef& struct_def) const {
    return WrapInNameSpace(
        struct_def.defined_namespace,
        ObjectApiName(struct_def.name, struct_def, parser_.opts));
  }

  std::string QualifiedFbsName(const Definition& def) const {
    return WrapInNameSpace(def.defined_namespace, def.name);
  }

  std::string QualifiedUnionNativeName(const EnumDef& union_def) const {
    return WrapInNameSpace(union_def.defined_namespace,
                           union_def.name + "Union");
  }

  static std::string UnrealScalarTypeName(const BaseType base_type) {
    switch (base_type) {
      case BASE_TYPE_BOOL: return "bool";
      case BASE_TYPE_CHAR: return "int8";
      case BASE_TYPE_UTYPE:
      case BASE_TYPE_UCHAR: return "uint8";
      case BASE_TYPE_SHORT: return "int16";
      case BASE_TYPE_USHORT: return "uint16";
      case BASE_TYPE_INT: return "int32";
      case BASE_TYPE_UINT: return "uint32";
      case BASE_TYPE_LONG: return "int64";
      case BASE_TYPE_ULONG: return "uint64";
      case BASE_TYPE_FLOAT: return "float";
      case BASE_TYPE_DOUBLE: return "double";
      default: return "";
    }
  }

  static std::string UnrealFieldScalarTypeName(const BaseType base_type) {
    switch (base_type) {
      case BASE_TYPE_CHAR:
      case BASE_TYPE_SHORT:
      case BASE_TYPE_USHORT: return "int32";
      case BASE_TYPE_UINT: return "int64";
      default: return UnrealScalarTypeName(base_type);
    }
  }

  static bool IsWidenedInteger(const Type& type) {
    return UnrealFieldScalarTypeName(type.base_type) !=
           UnrealScalarTypeName(type.base_type);
  }

  static Type LeafType(const Type& type) {
    return IsVector(type) ? type.VectorType() : type;
  }

  static bool IsEnumLeaf(const Type& type) {
    return type.enum_def != nullptr && IsInteger(type.base_type) &&
           !type.enum_def->is_union;
  }

  static bool IsFlagsLeaf(const Type& type) {
    return IsEnumLeaf(type) && IsBitFlags(*type.enum_def);
  }

  static bool IsUnionLeaf(const Type& type) {
    return type.base_type == BASE_TYPE_UNION;
  }

  static bool IsBulkCopyLeaf(const FieldDef& field, const Type& leaf) {
    if (FixedPointFractionBits(field) != 0 || leaf.enum_def != nullptr) {
      return false;
    }
    switch (leaf.base_type) {
      case BASE_TYPE_UCHAR:
      case BASE_TYPE_INT:
      case BASE_TYPE_LONG:
      case BASE_TYPE_ULONG:
      case BASE_TYPE_FLOAT:
      case BASE_TYPE_DOUBLE: return true;
      default: return false;
    }
  }

  static bool IsOptionalMember(const StructDef& struct_def,
                               const FieldDef& field) {
    if (field.IsScalarOptional()) return true;
    return !struct_def.fixed &&
           field.value.type.base_type == BASE_TYPE_STRUCT &&
           !field.IsRequired();
  }

  static std::string FlagsMemberTypeName(const EnumDef& enum_def) {
    switch (SizeOf(enum_def.underlying_type.base_type)) {
      case 1: return "uint8";
      case 8: return "int64";
      default: return "int32";
    }
  }

  static std::string UnrealLeafTypeName(const Type& type) {
    if (IsString(type)) return "FString";
    if (IsUnionLeaf(type)) return "FInstancedStruct";
    if (type.base_type == BASE_TYPE_STRUCT) {
      return UnrealStructName(*type.struct_def);
    }
    if (IsFlagsLeaf(type)) return FlagsMemberTypeName(*type.enum_def);
    if (IsEnumLeaf(type)) return UnrealEnumName(*type.enum_def);
    return UnrealFieldScalarTypeName(type.base_type);
  }

  static std::string UnrealElementTypeName(const FieldDef& field) {
    if (FixedPointFractionBits(field) != 0) return "double";
    return UnrealLeafTypeName(LeafType(field.value.type));
  }

  static std::string UnrealFieldTypeName(const StructDef& struct_def,
                                         const FieldDef& field) {
    std::string element = UnrealElementTypeName(field);
    if (IsVector(field.value.type)) return "TArray<" + element + ">";
    if (IsOptionalMember(struct_def, field)) {
      return "TOptional<" + element + ">";
    }
    return element;
  }

  std::string NativeElementTypeName(const Type& element) const {
    if (IsString(element)) return "std::string";
    if (IsUnionLeaf(element)) {
      return QualifiedUnionNativeName(*element.enum_def);
    }
    if (IsTable(element)) {
      return "std::unique_ptr<" + QualifiedNativeName(*element.struct_def) +
             ">";
    }
    if (IsStruct(element)) return QualifiedFbsName(*element.struct_def);
    if (IsEnumLeaf(element)) return QualifiedFbsName(*element.enum_def);
    return NativeScalarTypeName(element.base_type);
  }

  std::string TableElementTypeName(const Type& element) const {
    if (IsString(element)) return "::flatbuffers::String*";
    if (element.base_type == BASE_TYPE_STRUCT) {
      return QualifiedFbsName(*element.struct_def) + "*";
    }
    if (IsEnumLeaf(element)) return QualifiedFbsName(*element.enum_def);
    if (IsBool(element.base_type)) return "uint8_t";
    return NativeScalarTypeName(element.base_type);
  }

  static std::string LoopVariable(const Type& element,
                                  const std::string& type_name) {
    return "const " + type_name + (IsScalar(element.base_type) ? "" : "&") +
           " Elem";
  }

  static bool EnumValuesFitUint8(const EnumDef& enum_def) {
    for (const EnumVal* ev : enum_def.Vals()) {
      if (enum_def.underlying_type.base_type == BASE_TYPE_ULONG) {
        if (ev->GetAsUInt64() > 255) return false;
      } else {
        const int64_t value = ev->GetAsInt64();
        if (value < 0 || value > 255) return false;
      }
    }
    return true;
  }

  static bool IsBlueprintEnum(const EnumDef& enum_def) {
    if (!EnumValuesFitUint8(enum_def)) return false;
    if (IsBitFlags(enum_def) && enum_def.Lookup("None") == nullptr) return true;
    return enum_def.FindByValue("0") != nullptr;
  }

  static bool IsContiguousFromZero(const EnumDef& enum_def) {
    int64_t expected = 0;
    for (const EnumVal* ev : enum_def.Vals()) {
      if (ev->GetAsInt64() != expected) return false;
      ++expected;
    }
    return true;
  }

  static bool IsBlueprintCompatibleLeaf(const Type& type) {
    if (IsFlagsLeaf(type)) return true;
    if (IsEnumLeaf(type)) return IsBlueprintEnum(*type.enum_def);
    return type.base_type != BASE_TYPE_ULONG;
  }

  static bool IsBlueprintCompatible(const StructDef& struct_def,
                                    const FieldDef& field) {
    if (IsOptionalMember(struct_def, field)) return false;
    return IsBlueprintCompatibleLeaf(LeafType(field.value.type));
  }

  static std::vector<const EnumVal*> UnionMembers(const EnumDef& union_def) {
    std::vector<const EnumVal*> members;
    for (const EnumVal* ev : union_def.Vals()) {
      if (ev->union_type.base_type != BASE_TYPE_NONE) members.push_back(ev);
    }
    return members;
  }

  std::string UnionAllowedClasses(const EnumDef& union_def) const {
    std::vector<std::string> paths;
    for (const EnumVal* ev : UnionMembers(union_def)) {
      paths.push_back(StructScriptPath(*ev->union_type.struct_def));
    }
    return Join(paths, ", ");
  }

  static std::vector<const EnumDef*> UnionsOf(const StructDef& struct_def) {
    std::vector<const EnumDef*> unions;
    for (const FieldDef* field : struct_def.fields.vec) {
      if (SkipField(*field)) continue;
      const Type leaf = LeafType(field->value.type);
      if (IsUnionLeaf(leaf) && std::find(unions.begin(), unions.end(),
                                         leaf.enum_def) == unions.end()) {
        unions.push_back(leaf.enum_def);
      }
    }
    return unions;
  }

  static std::string UnionFromTableName(const EnumDef& union_def) {
    return union_def.name + "FromTable";
  }

  static std::string UnionFromNativeName(const EnumDef& union_def) {
    return union_def.name + "FromNative";
  }

  static std::string UnionToNativeName(const EnumDef& union_def) {
    return union_def.name + "ToNative";
  }

  std::string PropertySpecifiers(const StructDef& struct_def,
                                 const FieldDef& field) const {
    std::string specifiers = IsBlueprintCompatible(struct_def, field)
                                 ? "EditAnywhere, BlueprintReadWrite"
                                 : "EditAnywhere";
    const Type leaf = LeafType(field.value.type);
    if (IsFlagsLeaf(leaf)) {
      specifiers += ", Meta = (Bitmask, BitmaskEnum = \"" +
                    EnumScriptPath(*leaf.enum_def) + "\")";
    } else if (IsUnionLeaf(leaf)) {
      specifiers += ", Meta = (AllowedClasses = \"" +
                    UnionAllowedClasses(*leaf.enum_def) + "\")";
    }
    return specifiers;
  }

  static std::string FlagsDefault(const std::string& constant,
                                  const EnumDef& enum_def) {
    uint64_t value = 0;
    StringToNumber(constant.c_str(), &value);
    const std::string member_type = FlagsMemberTypeName(enum_def);
    if (member_type == "int32" &&
        value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      return "static_cast<int32>(" + constant + "U)";
    }
    if (member_type == "int64" &&
        value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return "static_cast<int64>(" + constant + "ULL)";
    }
    return constant;
  }

  std::string UnrealFieldDefault(const StructDef& struct_def,
                                 const FieldDef& field) {
    const Type& type = field.value.type;
    if (IsString(type) || IsVector(type) ||
        type.base_type == BASE_TYPE_STRUCT || IsUnionLeaf(type) ||
        IsOptionalMember(struct_def, field)) {
      return "";
    }
    const int fraction_bits = FixedPointFractionBits(field);
    if (fraction_bits != 0) {
      if (field.value.constant == "0") return " = 0.0";
      return " = " + NumToStringCpp(field.value.constant, type.base_type) +
             " / " + FixedPointScale(fraction_bits);
    }
    if (IsFlagsLeaf(type)) {
      return " = " + FlagsDefault(field.value.constant, *type.enum_def);
    }
    if (IsEnumLeaf(type)) {
      const EnumDef& enum_def = *type.enum_def;
      if (const EnumVal* ev = enum_def.FindByValue(field.value.constant)) {
        return " = " + UnrealEnumName(enum_def) + "::" + ev->name;
      }
      return " = static_cast<" + UnrealEnumName(enum_def) + ">(" +
             field.value.constant + ")";
    }
    if (IsBool(type.base_type)) {
      return field.value.constant == "0" ? " = false" : " = true";
    }
    if (IsFloat(type.base_type)) {
      const std::string constant = float_const_gen_.GenFloatConstant(field);
      if (constant.find("std::numeric_limits") != std::string::npos) {
        uses_numeric_limits_ = true;
      }
      return " = " + constant;
    }
    return " = " + NumToStringCpp(field.value.constant, type.base_type);
  }

  static bool SkipField(const FieldDef& field) {
    if (field.deprecated) return true;
    const Type& type = field.value.type;
    if (type.base_type == BASE_TYPE_UTYPE) return true;
    if (IsVector(type) && type.element == BASE_TYPE_UTYPE) return true;
    return false;
  }

  void CollectDefinitions() {
    for (const EnumDef* enum_def : parser_.enums_.vec) {
      if (!enum_def->generated && HasUnrealAttribute(*enum_def)) {
        marked_enums_.push_back(enum_def);
      }
    }
    for (const StructDef* struct_def : parser_.structs_.vec) {
      if (!struct_def->generated && HasUnrealAttribute(*struct_def)) {
        marked_structs_.push_back(struct_def);
      }
    }
  }

  void AddError(const Definition& def, const std::string& kind,
                const std::string& message) {
    errors_.push_back(StripPath(def.file) + ": " + kind + " '" +
                      def.defined_namespace->GetFullyQualifiedName(def.name) +
                      "' (unreal_type): " + message);
  }

  static std::string DescribeDefinition(const Definition& def,
                                        const std::string& kind) {
    return kind + " '" +
           def.defined_namespace->GetFullyQualifiedName(def.name) + "'";
  }

  void ValidateNameOverride(const Definition& def, const std::string& kind,
                            const char required_prefix) {
    const Value* attr = UnrealAttribute(def);
    if (attr == nullptr) return;
    if (attr->type.base_type == BASE_TYPE_NONE) return;
    if (attr->type.base_type != BASE_TYPE_STRING) {
      AddError(def, kind, "the attribute value must be a string type name");
      return;
    }
    if (attr->constant.empty()) {
      AddError(def, kind, "the type name override must not be empty");
      return;
    }
    if (required_prefix != 0 && attr->constant[0] != required_prefix) {
      AddError(def, kind,
               std::string("the type name override must start with '") +
                   required_prefix + "' (UnrealHeaderTool naming rule)");
    }
  }

  void ValidateSpecifiersUsage() {
    const auto check_value = [&](const Definition& def, const std::string& kind,
                                 const Value& attribute) {
      if (attribute.type.base_type != BASE_TYPE_STRING &&
          attribute.type.base_type != BASE_TYPE_NONE) {
        AddError(def, kind, "the unreal_specifiers attribute must be a string");
      }
    };
    for (const EnumDef* enum_def : parser_.enums_.vec) {
      if (enum_def->generated) continue;
      const std::string kind = enum_def->is_union ? "union" : "enum";
      if (const Value* attribute = SpecifiersAttribute(enum_def->attributes)) {
        if (!HasUnrealAttribute(*enum_def)) {
          AddError(*enum_def, kind,
                   "the unreal_specifiers attribute requires unreal_type");
        }
        check_value(*enum_def, kind, *attribute);
      }
      for (const EnumVal* ev : enum_def->Vals()) {
        if (SpecifiersAttribute(ev->attributes) != nullptr) {
          AddError(*enum_def, kind,
                   "enumerator '" + ev->name +
                       "' carries unreal_specifiers; the attribute applies to "
                       "enums, tables, structs and fields");
        }
      }
    }
    for (const StructDef* struct_def : parser_.structs_.vec) {
      if (struct_def->generated) continue;
      const std::string kind = struct_def->fixed ? "struct" : "table";
      if (const Value* attribute =
              SpecifiersAttribute(struct_def->attributes)) {
        if (!HasUnrealAttribute(*struct_def)) {
          AddError(*struct_def, kind,
                   "the unreal_specifiers attribute requires unreal_type");
        }
        check_value(*struct_def, kind, *attribute);
      }
      for (const FieldDef* field : struct_def->fields.vec) {
        if (const Value* attribute = SpecifiersAttribute(field->attributes)) {
          if (!HasUnrealAttribute(*struct_def)) {
            AddError(*struct_def, kind,
                     "field '" + field->name +
                         "' has unreal_specifiers but the " + kind +
                         " but does not have unreal_type");
          }
          check_value(*struct_def, kind, *attribute);
        }
      }
    }
  }

  void ValidateFixedPointUsage() {
    for (const StructDef* struct_def : parser_.structs_.vec) {
      if (struct_def->generated) continue;
      const std::string kind = struct_def->fixed ? "struct" : "table";
      for (const FieldDef* field : struct_def->fields.vec) {
        const Value* attribute = FixedPointAttribute(*field);
        if (attribute == nullptr) continue;
        const auto field_error = [&](const std::string& message) {
          AddError(*struct_def, kind, "field '" + field->name + "' " + message);
        };
        const Type leaf = LeafType(field->value.type);
        if (!IsInteger(leaf.base_type) || IsBool(leaf.base_type) ||
            leaf.enum_def != nullptr) {
          field_error(
              "carries fixed_point but is not an integer or a vector of "
              "integers");
          continue;
        }
        const int bit_width = static_cast<int>(SizeOf(leaf.base_type)) * 8;
        if (bit_width > 32) {
          field_error(
              "has fixed_point on a 64-bit integer, which a double "
              "cannot hold exactly");
          continue;
        }
        const int fraction_bits = FixedPointFractionBits(*field);
        if (fraction_bits < 1 || fraction_bits > bit_width) {
          field_error("needs its fraction bit count as the fixed_point value, "
                      "from 1 to " +
                      NumToString(bit_width) + ", e.g. (fixed_point: 8)");
        }
      }
    }
  }

  void Validate() {
    CheckUnrealNameCollisions();
    for (const EnumDef* enum_def : marked_enums_) {
      ValidateEnum(*enum_def);
    }
    for (const StructDef* struct_def : marked_structs_) {
      ValidateStruct(*struct_def);
    }
  }

  void ValidateEnum(const EnumDef& enum_def) {
    if (enum_def.is_union) {
      AddError(enum_def, "union",
               "unions are not marked; a union field becomes an "
               "FInstancedStruct restricted to the union's members, which "
               "must be marked tables or structs");
      return;
    }
    ValidateNameOverride(enum_def, "enum", 0);
  }

  static bool IsReservedMemberName(const std::string& member) {
    static const char* const kReserved[] = {
        "ToNative", "FromNative", "FromTable", "StaticStruct", "Native",
        "InNative", "InTable",    "InValue",   "InType",       "Result",
        "Elem",     "ElemIndex"};
    for (const char* reserved : kReserved) {
      if (member == reserved) return true;
    }
    return false;
  }

  void ValidateStruct(const StructDef& struct_def) {
    const std::string kind = struct_def.fixed ? "struct" : "table";
    ValidateNameOverride(struct_def, kind, 'F');
    if (struct_def.attributes.Lookup("native_type") != nullptr) {
      AddError(struct_def, kind,
               "the native_type attribute replaces the object-API type the "
               "conversions target and is not supported");
    }
    if (struct_def.attributes.Lookup("native_custom_alloc") != nullptr) {
      AddError(struct_def, kind,
               "the native_custom_alloc attribute is not supported");
    }

    std::map<std::string, std::string> member_names;
    for (const FieldDef* field : struct_def.fields.vec) {
      if (SkipField(*field)) continue;
      ValidateField(struct_def, kind, *field);

      const std::string member = UnrealFieldName(*field);
      if (IsReservedMemberName(member)) {
        AddError(struct_def, kind,
                 "field '" + field->name + "' maps to the reserved member "
                 "name '" + member + "'");
      }
      const auto inserted = member_names.emplace(member, field->name);
      if (!inserted.second) {
        AddError(struct_def, kind,
                 "fields '" + inserted.first->second + "' and '" +
                     field->name + "' both map to the member name '" + member +
                     "'");
      }
    }
    if (struct_def.fixed) return;

    std::map<std::string, const EnumDef*> union_names;
    for (const EnumDef* union_def : UnionsOf(struct_def)) {
      const auto inserted = union_names.emplace(union_def->name, union_def);
      if (!inserted.second) {
        AddError(struct_def, kind,
                 "holds unions '" + QualifiedFbsName(*inserted.first->second) +
                     "' and '" + QualifiedFbsName(*union_def) +
                     "', whose conversion helpers would share names");
      }
      const std::string helpers[] = {UnionFromTableName(*union_def),
                                     UnionFromNativeName(*union_def),
                                     UnionToNativeName(*union_def)};
      for (const std::string& helper : helpers) {
        const auto it = member_names.find(helper);
        if (it != member_names.end()) {
          AddError(struct_def, kind,
                   "field '" + it->second + "' maps to the member name '" +
                       helper + "', the conversion helper for union '" +
                       union_def->name + "'");
        }
      }
    }
  }

  void ValidateField(const StructDef& struct_def, const std::string& kind,
                     const FieldDef& field) {
    const Type& type = field.value.type;
    const auto field_error = [&](const std::string& message) {
      AddError(struct_def, kind, "field '" + field.name + "' " + message);
    };

    if (IsArray(type)) {
      field_error("is a fixed-size array; arrays are not supported");
      return;
    }
    if (field.flexbuffer) {
      field_error("is a flexbuffer; flexbuffers are not supported");
      return;
    }
    if (field.offset64 || type.base_type == BASE_TYPE_VECTOR64) {
      field_error("uses 64-bit offsets; offset64/vector64 are not supported");
      return;
    }
    if (field.native_inline) {
      field_error(
          "uses native_inline, which changes the object-API representation "
          "the conversions target");
      return;
    }
    if (field.attributes.Lookup("cpp_type") != nullptr ||
        field.attributes.Lookup("hash") != nullptr) {
      field_error(
          "uses cpp_type/hash, which changes the object-API representation "
          "the conversions target");
      return;
    }
    if (field.attributes.Lookup("cpp_str_type") != nullptr ||
        field.attributes.Lookup("cpp_ptr_type") != nullptr) {
      field_error(
          "uses cpp_str_type/cpp_ptr_type, which changes the object-API "
          "representation the conversions target");
      return;
    }

    const Type leaf = LeafType(type);
    if (IsUnionLeaf(leaf)) {
      for (const EnumVal* ev : UnionMembers(*leaf.enum_def)) {
        if (ev->union_type.base_type != BASE_TYPE_STRUCT) {
          field_error("holds " + DescribeDefinition(*leaf.enum_def, "union") +
                      ", whose member '" + ev->name +
                      "' is not a table or struct; an FInstancedStruct holds "
                      "only marked tables and structs");
        } else if (!HasUnrealAttribute(*ev->union_type.struct_def)) {
          const StructDef& member_def = *ev->union_type.struct_def;
          field_error("holds " + DescribeDefinition(*leaf.enum_def, "union") +
                      ", whose member " +
                      DescribeDefinition(member_def,
                                         member_def.fixed ? "struct" : "table") +
                      " is not marked unreal_type");
        }
      }
      if (unreal_module_.empty()) {
        field_error(
            "holds a union; pass --unreal-module so its AllowedClasses "
            "metadata can name the member structs");
      }
      return;
    }
    if (IsEnumLeaf(leaf) && !HasUnrealAttribute(*leaf.enum_def)) {
      field_error("references " +
                  DescribeDefinition(*leaf.enum_def, "enum") +
                  " which is not marked unreal_type");
    }
    if (IsFlagsLeaf(leaf) && unreal_module_.empty()) {
      field_error(
          "is a bit_flags enum; pass --unreal-module so its BitmaskEnum "
          "metadata can name the enum");
    }
    if (leaf.base_type == BASE_TYPE_STRUCT && !HasUnrealAttribute(*leaf.struct_def)) {
      const StructDef& referenced = *leaf.struct_def;
      field_error("references " +
                  DescribeDefinition(referenced,
                                     referenced.fixed ? "struct" : "table") +
                  " which is not marked unreal_type");
    }
  }

  void CheckUnrealNameCollisions() {
    std::map<std::string, const Definition*> names;
    const auto check = [&](const Definition& def, const std::string& kind,
                           const std::string& ue_name) {
      const auto inserted = names.emplace(ue_name, &def);
      if (!inserted.second) {
        AddError(def, kind,
                 "generated type name '" + ue_name + "' collides with the "
                 "one generated for '" + inserted.first->second->name +
                 "'; use a name override, e.g. (unreal_type: \"" + ue_name +
                 "2\")");
      }
    };
    for (const EnumDef* enum_def : marked_enums_) {
      check(*enum_def, "enum", UnrealEnumName(*enum_def));
    }
    for (const StructDef* struct_def : marked_structs_) {
      check(*struct_def, struct_def->fixed ? "struct" : "table",
            UnrealStructName(*struct_def));
    }
  }

  std::vector<const StructDef*> SortStructsByDependency() {
    enum class VisitState { kUnvisited, kInProgress, kDone };
    std::map<const StructDef*, VisitState> states;
    for (const StructDef* struct_def : marked_structs_) {
      states[struct_def] = VisitState::kUnvisited;
    }

    std::vector<const StructDef*> sorted;
    sorted.reserve(marked_structs_.size());
    std::function<void(const StructDef*)> visit =
        [&](const StructDef* struct_def) {
          states[struct_def] = VisitState::kInProgress;
          for (const FieldDef* field : struct_def->fields.vec) {
            if (SkipField(*field)) continue;
            const Type leaf = LeafType(field->value.type);
            if (leaf.base_type != BASE_TYPE_STRUCT) continue;
            const StructDef* dependency = leaf.struct_def;
            const auto it = states.find(dependency);
            if (it == states.end()) continue;
            if (it->second == VisitState::kInProgress) {
              AddError(*struct_def, struct_def->fixed ? "struct" : "table",
                       "is part of a reference cycle with '" +
                           dependency->name +
                           "'; cyclic types are not supported");
              continue;
            }
            if (it->second == VisitState::kUnvisited) visit(dependency);
          }
          states[struct_def] = VisitState::kDone;
          sorted.push_back(struct_def);
        };

    for (const StructDef* struct_def : marked_structs_) {
      if (states[struct_def] == VisitState::kUnvisited) visit(struct_def);
    }
    return sorted;
  }

  std::string UnrealGeneratedBaseName() const {
    return StripExtension(
        StripPath(GeneratedFileName("", file_name_ + "_unreal", parser_.opts)));
  }

  void CollectCrossFileIncludes(std::set<std::string>* includes) const {
    const std::vector<IncludedFile> included_files = parser_.GetIncludedFiles();
    const auto add_for = [&](const Definition& def) {
      if (!def.generated) return;
      std::string schema_name;
      for (const IncludedFile& included_file : included_files) {
        if (included_file.filename == def.file) {
          schema_name = included_file.schema_name;
          break;
        }
      }
      if (schema_name.empty()) schema_name = def.file;
      includes->insert(GeneratedFileName(
          "", StripPath(StripExtension(schema_name)) + "_unreal",
          parser_.opts));
    };

    for (const StructDef* struct_def : marked_structs_) {
      for (const FieldDef* field : struct_def->fields.vec) {
        if (SkipField(*field)) continue;
        const Type leaf = LeafType(field->value.type);
        if (IsEnumLeaf(leaf)) add_for(*leaf.enum_def);
        if (leaf.base_type == BASE_TYPE_STRUCT) add_for(*leaf.struct_def);
        if (IsUnionLeaf(leaf)) {
          for (const EnumVal* ev : UnionMembers(*leaf.enum_def)) {
            add_for(*ev->union_type.struct_def);
          }
        }
      }
    }
  }

  void GenPreamble() {
    code_ += "// " + std::string(FlatBuffersGeneratedWarning());
    code_ +=
        "// UnrealHeaderTool-reflected mirror types generated by flatc "
        "--cpp-unreal.";
    code_ += "";
    code_ += "#pragma once";
    code_ += "";
    code_ += "#include \"CoreMinimal.h\"";
    if (uses_enum_class_flags_) code_ += "#include \"Misc/EnumClassFlags.h\"";
    if (uses_enum_range_) code_ += "#include \"Misc/EnumRange.h\"";
    if (uses_instanced_struct_) {
      code_ += "#include \"StructUtils/InstancedStruct.h\"";
    }
    code_ += "#include \"UObject/ObjectMacros.h\"";
    code_ += "";
    if (uses_numeric_limits_ || uses_std_memory_ || uses_std_string_) {
      if (uses_numeric_limits_) code_ += "#include <limits>";
      if (uses_std_memory_) code_ += "#include <memory>";
      if (uses_std_string_) code_ += "#include <string>";
      code_ += "";
    }
    code_ += "#include \"" +
             GeneratedFileName(parser_.opts.include_prefix, file_name_,
                               parser_.opts) +
             "\"";

    std::set<std::string> cross_file_includes;
    CollectCrossFileIncludes(&cross_file_includes);
    for (const std::string& include : cross_file_includes) {
      code_ += "#include \"" + include + "\"";
    }
    code_ += "";
    code_ += "#include \"" + UnrealGeneratedBaseName() + ".generated.h\"";
    code_ += "";
  }

  void GenEnumeratorList(const std::vector<std::string>& enumerators) {
    code_.IncrementIdentLevel();
    for (size_t i = 0; i < enumerators.size(); ++i) {
      code_ += enumerators[i] + (i + 1 < enumerators.size() ? "," : "");
    }
    code_.DecrementIdentLevel();
  }

  void GenEnumRangeCheck(const std::string& checked, const std::string& shown,
                         const std::string& fbs_name,
                         const std::string& shown_type) {
    code_ += "checkf(!::flatbuffers::IsOutRange(" + checked + ", " + fbs_name +
             "::MIN, " + fbs_name + "::MAX), TEXT(\"Invalid " + shown_type +
             " %lld\"), static_cast<int64>(" + shown + "));";
  }

  void GenEnumText(const EnumDef& enum_def, const std::string& ue_name,
                   const bool emit_none, const bool emit_any,
                   const bool emit_count) {
    const auto empty_case = [&](const std::string& enumerator) {
      code_ += "case " + ue_name + "::" + enumerator +
               ": return FText::GetEmpty();";
    };
    code_ += "#define LOCTEXT_NAMESPACE \"" + ue_name + "\"";
    code_ += "";
    code_ += "inline FText ToText(const " + ue_name + " InValue)";
    code_ += "{";
    code_.IncrementIdentLevel();
    code_ += "switch (InValue)";
    code_ += "{";
    if (emit_none) empty_case("None");
    for (const EnumVal* ev : enum_def.Vals()) {
      const Value* english = ev->attributes.Lookup("english");
      const std::string text = english != nullptr && !english->constant.empty()
                                   ? english->constant
                                   : ev->name;
      code_ += "case " + ue_name + "::" + ev->name + ": return LOCTEXT(\"" +
               ev->name + "\", \"" + EscapeCppString(text) + "\");";
    }
    if (emit_any) empty_case("Any");
    if (emit_count) empty_case("Count");
    code_ += "default: return FText::GetEmpty();";
    code_ += "}";
    code_.DecrementIdentLevel();
    code_ += "}";
    code_ += "";
    code_ += "#undef LOCTEXT_NAMESPACE";
    code_ += "";
  }

  static std::string EnumSpecifiers(const EnumDef& enum_def,
                             const bool blueprint_type) {
    if (const Value* override_attribute =
            SpecifiersAttribute(enum_def.attributes)) {
      return SpecifierList(*override_attribute);
    }
    std::vector<std::string> specifiers;
    std::vector<std::string> meta;
    if (blueprint_type) specifiers.emplace_back("BlueprintType");
    if (IsBitFlags(enum_def)) {
      specifiers.emplace_back("Flags");
      meta.emplace_back("Bitflags");
      meta.emplace_back("UseEnumValuesAsMaskValuesInEditor = \"true\"");
    }
    if (!meta.empty()) specifiers.push_back("Meta = (" + Join(meta, ", ") + ")");
    return Join(specifiers, ", ");
  }

  void GenEnum(const EnumDef& enum_def) {
    const std::string ue_name = UnrealEnumName(enum_def);
    const std::string fbs_name = QualifiedFbsName(enum_def);
    const bool is_bit_flags = IsBitFlags(enum_def);
    const bool blueprint_type = IsBlueprintEnum(enum_def);
    const std::string underlying =
        EnumValuesFitUint8(enum_def)
            ? "uint8"
            : UnrealScalarTypeName(enum_def.underlying_type.base_type);
    const bool emit_count = !is_bit_flags && IsContiguousFromZero(enum_def) &&
                            enum_def.Lookup("Count") == nullptr;
    const bool emit_none = is_bit_flags && enum_def.Lookup("None") == nullptr;
    const bool emit_any = is_bit_flags && enum_def.Lookup("Any") == nullptr;
    uint64_t any_mask = 0;
    for (const EnumVal* ev : enum_def.Vals()) {
      any_mask |= ev->GetAsUInt64();
    }

    std::vector<std::string> enumerators;
    if (emit_none) {
      enumerators.emplace_back("None = 0 UMETA(Hidden)");
    }
    for (const EnumVal* ev : enum_def.Vals()) {
      std::string enumerator = ev->name + " = " + enum_def.ToString(*ev);
      const Value* english = ev->attributes.Lookup("english");
      if (english != nullptr && !english->constant.empty()) {
        enumerator += " UMETA(DisplayName = \"" +
                      EscapeCppString(english->constant) + "\")";
      }
      enumerators.push_back(enumerator);
    }
    if (emit_any) {
      enumerators.push_back("Any = " + NumToString(any_mask) +
                            " UMETA(Hidden)");
    }
    if (emit_count) {
      enumerators.emplace_back("Count UMETA(Hidden)");
    }

    code_ += "UENUM(" + EnumSpecifiers(enum_def, blueprint_type) + ")";
    code_ += "enum class " + ue_name + " : " + underlying;
    code_ += "{";
    GenEnumeratorList(enumerators);
    code_ += "};";
    code_ += "";

    const auto pin = [&](const std::string& ue_value,
                         const std::string& fbs_value) {
      code_ += "static_assert(static_cast<int64>(" + ue_name + "::" + ue_value +
               ") == static_cast<int64>(" + fbs_name + "::" + fbs_value +
               "), \"" + ue_name + "::" + ue_value + " must match " + fbs_name +
               "\");";
    };
    if (emit_none) pin("None", "NONE");
    for (const EnumVal* ev : enum_def.Vals()) {
      pin(ev->name, ev->name);
    }
    if (emit_any) pin("Any", "ANY");
    if (emit_count) {
      code_ += "static_assert(static_cast<int64>(" + ue_name +
               "::Count) == " + NumToString(enum_def.size()) + ", \"" +
               ue_name + " enumerator count changed\");";
    }
    code_ += "";

    if (is_bit_flags) {
      uses_enum_class_flags_ = true;
      code_ += "ENUM_CLASS_FLAGS(" + ue_name + ");";
      code_ += "";
    }

    uses_enum_range_ = true;
    if (emit_count) {
      code_ += "ENUM_RANGE_BY_COUNT(" + ue_name + ", " + ue_name + "::Count)";
    } else {
      std::vector<std::string> values;
      for (const EnumVal* ev : enum_def.Vals()) {
        values.push_back(ue_name + "::" + ev->name);
      }
      code_ += "ENUM_RANGE_BY_VALUES(" + ue_name + ", " + Join(values, ", ") +
               ")";
    }
    code_ += "";

    const std::string convert_name = ConvertFunctionName(enum_def);
    code_ += "inline " + ue_name + " " + convert_name + "(const " + fbs_name +
             " InValue)";
    code_ += "{";
    code_.IncrementIdentLevel();
    if (!is_bit_flags) {
      GenEnumRangeCheck("InValue", "InValue", fbs_name, fbs_name);
    }
    code_ += "return static_cast<" + ue_name + ">(InValue);";
    code_.DecrementIdentLevel();
    code_ += "}";
    code_ += "";

    code_ += "inline " + fbs_name + " " + convert_name + "(const " + ue_name +
             " InValue)";
    code_ += "{";
    code_.IncrementIdentLevel();
    if (is_bit_flags) {
      code_ += "return static_cast<" + fbs_name + ">(InValue);";
    } else {
      code_ += "const " + fbs_name + " Native = static_cast<" + fbs_name +
               ">(InValue);";
      GenEnumRangeCheck("Native", "InValue", fbs_name, ue_name);
      code_ += "return Native;";
    }
    code_.DecrementIdentLevel();
    code_ += "}";
    code_ += "";

    GenEnumText(enum_def, ue_name, emit_none, emit_any, emit_count);
  }

  void NoteFieldUsage(const FieldDef& field) {
    const Type& type = field.value.type;
    const Type leaf = LeafType(type);
    if (IsString(leaf)) uses_std_string_ = true;
    if (IsTable(leaf) || (IsStruct(type) && !IsVector(type))) {
      uses_std_memory_ = true;
    }
    if (IsUnionLeaf(leaf)) uses_instanced_struct_ = true;
  }

  void GenStructDecl(const StructDef& struct_def) {
    const std::string ue_name = UnrealStructName(struct_def);
    const std::string native_name = QualifiedNativeName(struct_def);
    const std::string fbs_name = QualifiedFbsName(struct_def);

    const Value* struct_specifiers =
        SpecifiersAttribute(struct_def.attributes);
    code_ += "USTRUCT(" +
             (struct_specifiers ? SpecifierList(*struct_specifiers)
                                : std::string("BlueprintType")) +
             ")";
    code_ += "struct " + ue_name;
    code_ += "{";
    code_.IncrementIdentLevel();
    code_ += "GENERATED_BODY()";
    code_ += "";
    for (const FieldDef* field : struct_def.fields.vec) {
      if (SkipField(*field)) continue;
      NoteFieldUsage(*field);
      const Value* field_specifiers = SpecifiersAttribute(field->attributes);
      code_ += "UPROPERTY(" +
               (field_specifiers ? SpecifierList(*field_specifiers)
                                 : PropertySpecifiers(struct_def, *field)) +
               ")";
      code_ += UnrealFieldTypeName(struct_def, *field) + " " +
               UnrealFieldName(*field) + UnrealFieldDefault(struct_def, *field) +
               ";";
      code_ += "";
    }
    code_ += native_name + " ToNative() const;";
    code_ += "static " + ue_name + " FromNative(const " + native_name +
             "& InNative);";
    if (!struct_def.fixed) {
      code_ += "static " + ue_name + " FromTable(const " + fbs_name +
               "& InTable);";
    }

    const std::vector<const EnumDef*> unions = UnionsOf(struct_def);
    if (!unions.empty()) {
      code_.DecrementIdentLevel();
      code_ += "";
      code_ += "private:";
      code_.IncrementIdentLevel();
      for (const EnumDef* union_def : unions) {
        const std::string union_name = QualifiedFbsName(*union_def);
        const std::string union_native = QualifiedUnionNativeName(*union_def);
        code_ += "static FInstancedStruct " + UnionFromTableName(*union_def) +
                 "(const " + union_name + " InType, const void* InValue);";
        code_ += "static FInstancedStruct " + UnionFromNativeName(*union_def) +
                 "(const " + union_native + "& InValue);";
        code_ += "static " + union_native + " " + UnionToNativeName(*union_def) +
                 "(const FInstancedStruct& InValue);";
      }
    }
    code_.DecrementIdentLevel();
    code_ += "};";
    code_ += "";
  }

  std::string FlagsToNativeExpr(const EnumDef& enum_def,
                                const std::string& source) const {
    const std::string fbs_name = QualifiedFbsName(enum_def);
    const BaseType underlying = enum_def.underlying_type.base_type;
    const std::string native_underlying = NativeScalarTypeName(underlying);
    switch (SizeOf(underlying)) {
      case 1: return "static_cast<" + fbs_name + ">(" + source + ")";
      case 2:
        return "static_cast<" + fbs_name + ">(IntCastChecked<" +
               native_underlying + ">(" + source + "))";
      default:
        return "static_cast<" + fbs_name + ">(static_cast<" +
               native_underlying + ">(" + source + "))";
    }
  }

  std::string ToNativeValueExpr(const Type& type, const std::string& source,
                                const int fraction_bits) const {
    if (IsString(type)) return "std::string(TCHAR_TO_UTF8(*" + source + "))";
    if (IsStruct(type)) return source + ".ToNative()";
    if (IsFlagsLeaf(type)) return FlagsToNativeExpr(*type.enum_def, source);
    if (IsEnumLeaf(type)) {
      return ConvertFunctionName(*type.enum_def) + "(" + source + ")";
    }
    if (fraction_bits != 0) {
      return "IntCastChecked<" + NativeScalarTypeName(type.base_type) +
             ">(static_cast<int64>(FMath::RoundHalfFromZero(" + source +
             " * " + FixedPointScale(fraction_bits) + ")))";
    }
    if (IsWidenedInteger(type)) {
      return "IntCastChecked<" + NativeScalarTypeName(type.base_type) + ">(" +
             source + ")";
    }
    return source;
  }

  static std::string FromNativeValueExpr(const Type& type,
                                         const std::string& source,
                                         const int fraction_bits) {
    if (IsString(type)) {
      return "FString(UTF8_TO_TCHAR(" + source + ".c_str()))";
    }
    if (IsStruct(type)) {
      return UnrealStructName(*type.struct_def) + "::FromNative(" + source +
             ")";
    }
    if (IsFlagsLeaf(type)) {
      return "static_cast<" + FlagsMemberTypeName(*type.enum_def) + ">(" +
             source + ")";
    }
    if (IsEnumLeaf(type)) {
      return ConvertFunctionName(*type.enum_def) + "(" + source + ")";
    }
    if (fraction_bits != 0) {
      return "static_cast<double>(" + source + ") / " +
             FixedPointScale(fraction_bits);
    }
    return source;
  }

  static std::string FromTableElementExpr(const Type& element,
                                          const std::string& source,
                                          const int fraction_bits) {
    if (IsString(element)) {
      return "FString(UTF8_TO_TCHAR(" + source + "->c_str()))";
    }
    if (IsTable(element)) {
      return UnrealStructName(*element.struct_def) + "::FromTable(*" + source +
             ")";
    }
    if (IsStruct(element)) {
      return UnrealStructName(*element.struct_def) + "::FromNative(*" +
             source + ")";
    }
    if (IsFlagsLeaf(element)) {
      return "static_cast<" + FlagsMemberTypeName(*element.enum_def) + ">(" +
             source + ")";
    }
    if (IsEnumLeaf(element)) {
      return ConvertFunctionName(*element.enum_def) + "(" + source + ")";
    }
    if (IsBool(element.base_type)) return source + " != 0";
    if (fraction_bits != 0) {
      return "static_cast<double>(" + source + ") / " +
             FixedPointScale(fraction_bits);
    }
    return source;
  }

  void OpenBlock(const std::string& statement) {
    code_ += statement;
    code_ += "{";
    code_.IncrementIdentLevel();
  }

  void CloseBlock() {
    code_.DecrementIdentLevel();
    code_ += "}";
  }

  void GenTableFieldToNative(const StructDef& struct_def,
                             const FieldDef& field) {
    const Type& type = field.value.type;
    const std::string member = UnrealFieldName(field);
    const std::string target = "Native." + field.name;
    const int fraction_bits = FixedPointFractionBits(field);
    if (IsVector(type)) {
      const Type element = type.VectorType();
      if (IsBulkCopyLeaf(field, element)) {
        code_ += target + ".assign(" + member + ".GetData(), " + member +
                 ".GetData() + " + member + ".Num());";
        return;
      }
      code_ += target + ".reserve(static_cast<size_t>(" + member + ".Num()));";
      OpenBlock("for (" + LoopVariable(element, UnrealElementTypeName(field)) +
                " : " + member + ")");
      if (IsUnionLeaf(element)) {
        code_ += target + ".push_back(" + UnionToNativeName(*element.enum_def) +
                 "(Elem));";
      } else if (IsTable(element)) {
        code_ += target + ".push_back(std::make_unique<" +
                 QualifiedNativeName(*element.struct_def) +
                 ">(Elem.ToNative()));";
      } else {
        code_ += target + ".push_back(" +
                 ToNativeValueExpr(element, "Elem", fraction_bits) + ");";
      }
      CloseBlock();
      return;
    }
    if (IsUnionLeaf(type)) {
      code_ += target + " = " + UnionToNativeName(*type.enum_def) + "(" +
               member + ");";
      return;
    }
    const bool optional = IsOptionalMember(struct_def, field);
    const std::string value = optional ? member + ".GetValue()" : member;
    if (optional) OpenBlock("if (" + member + ".IsSet())");
    if (type.base_type == BASE_TYPE_STRUCT) {
      const std::string native_type = IsTable(type)
                                          ? QualifiedNativeName(*type.struct_def)
                                          : QualifiedFbsName(*type.struct_def);
      code_ += target + " = std::make_unique<" + native_type + ">(" + value +
               ".ToNative());";
    } else {
      code_ += target + " = " + ToNativeValueExpr(type, value, fraction_bits) +
               ";";
    }
    if (optional) CloseBlock();
  }

  void GenTableFieldFromNative(const StructDef& struct_def,
                               const FieldDef& field) {
    const Type& type = field.value.type;
    const std::string member = "Result." + UnrealFieldName(field);
    const std::string source = "InNative." + field.name;
    const int fraction_bits = FixedPointFractionBits(field);
    if (IsVector(type)) {
      const Type element = type.VectorType();
      if (IsBulkCopyLeaf(field, element)) {
        code_ += member + ".Append(MakeArrayView(" + source +
                 ".data(), static_cast<int32>(" + source + ".size())));";
        return;
      }
      code_ += member + ".Reserve(static_cast<int32>(" + source + ".size()));";
      OpenBlock("for (" + LoopVariable(element, NativeElementTypeName(element)) +
                " : " + source + ")");
      if (IsUnionLeaf(element)) {
        code_ += member + ".Add(" + UnionFromNativeName(*element.enum_def) +
                 "(Elem));";
      } else if (IsTable(element)) {
        code_ += member + ".Add(" + UnrealStructName(*element.struct_def) +
                 "::FromNative(*Elem));";
      } else {
        code_ += member + ".Add(" +
                 FromNativeValueExpr(element, "Elem", fraction_bits) + ");";
      }
      CloseBlock();
      return;
    }
    if (IsUnionLeaf(type)) {
      code_ += member + " = " + UnionFromNativeName(*type.enum_def) + "(" +
               source + ");";
      return;
    }
    if (field.IsScalarOptional()) {
      OpenBlock("if (" + source + ".has_value())");
      code_ += member + " = " +
               FromNativeValueExpr(type, "*" + source, fraction_bits) + ";";
      CloseBlock();
      return;
    }
    if (type.base_type == BASE_TYPE_STRUCT) {
      const std::string conversion = UnrealStructName(*type.struct_def) +
                                     "::FromNative(*" + source + ")";
      if (IsOptionalMember(struct_def, field)) {
        OpenBlock("if (" + source + ")");
        code_ += member + " = " + conversion + ";";
        CloseBlock();
      } else {
        code_ += member + " = " + conversion + ";";
      }
      return;
    }
    code_ += member + " = " + FromNativeValueExpr(type, source, fraction_bits) +
             ";";
  }

  void GenTableFieldFromTable(const FieldDef& field) {
    const Type& type = field.value.type;
    const std::string member = "Result." + UnrealFieldName(field);
    const std::string accessor = "InTable." + field.name + "()";
    const int fraction_bits = FixedPointFractionBits(field);
    if (IsUnionLeaf(type)) {
      code_ += member + " = " + UnionFromTableName(*type.enum_def) +
               "(InTable." + field.sibling_union_field->name + "(), " +
               accessor + ");";
      return;
    }
    if (field.IsScalarOptional()) {
      OpenBlock("if (" + accessor + ".has_value())");
      code_ += member + " = " +
               FromNativeValueExpr(type, "*" + accessor, fraction_bits) + ";";
      CloseBlock();
      return;
    }
    const bool nullable = !IsScalar(type.base_type) && !field.IsRequired();
    if (nullable) OpenBlock("if (" + accessor + ")");
    if (IsVector(type)) {
      const Type element = type.VectorType();
      if (IsUnionLeaf(element)) {
        code_ += member + ".Reserve(static_cast<int32>(" + accessor +
                 "->size()));";
        OpenBlock("for (::flatbuffers::uoffset_t ElemIndex = 0; ElemIndex < " +
                  accessor + "->size(); ++ElemIndex)");
        code_ += member + ".Add(" + UnionFromTableName(*element.enum_def) +
                 "(InTable." + field.sibling_union_field->name +
                 "()->Get(ElemIndex), " + accessor + "->Get(ElemIndex)));";
        CloseBlock();
      } else if (IsBulkCopyLeaf(field, element)) {
        code_ += member + ".Append(MakeArrayView(" + accessor +
                 "->data(), static_cast<int32>(" + accessor + "->size())));";
      } else {
        code_ += member + ".Reserve(static_cast<int32>(" + accessor +
                 "->size()));";
        OpenBlock("for (const " + TableElementTypeName(element) + " Elem : *" +
                  accessor + ")");
        code_ += member + ".Add(" +
                 FromTableElementExpr(element, "Elem", fraction_bits) + ");";
        CloseBlock();
      }
    } else if (IsString(type)) {
      code_ += member + " = UTF8_TO_TCHAR(" + accessor + "->c_str());";
    } else if (IsTable(type)) {
      code_ += member + " = " + UnrealStructName(*type.struct_def) +
               "::FromTable(*" + accessor + ");";
    } else if (IsStruct(type)) {
      code_ += member + " = " + UnrealStructName(*type.struct_def) +
               "::FromNative(*" + accessor + ");";
    } else {
      code_ += member + " = " +
               FromNativeValueExpr(type, accessor, fraction_bits) + ";";
    }
    if (nullable) CloseBlock();
  }

  void GenUnionHelpers(const StructDef& struct_def, const EnumDef& union_def) {
    const std::string ue_name = UnrealStructName(struct_def);
    const std::string union_name = QualifiedFbsName(union_def);
    const std::string union_native = QualifiedUnionNativeName(union_def);
    const std::vector<const EnumVal*> members = UnionMembers(union_def);

    code_ += "inline FInstancedStruct " + ue_name +
             "::" + UnionFromTableName(union_def) + "(const " + union_name +
             " InType, const void* InValue)";
    code_ += "{";
    code_.IncrementIdentLevel();
    code_ += "switch (InType)";
    code_ += "{";
    for (const EnumVal* ev : members) {
      const StructDef& member_def = *ev->union_type.struct_def;
      const std::string member_ue = UnrealStructName(member_def);
      code_ += "case " + union_name + "::" + ev->name + ":";
      code_.IncrementIdentLevel();
      code_ += "return FInstancedStruct::Make<" + member_ue + ">(" +
               member_ue + (member_def.fixed ? "::FromNative" : "::FromTable") +
               "(*static_cast<const " + QualifiedFbsName(member_def) +
               "*>(InValue)));";
      code_.DecrementIdentLevel();
    }
    code_ += "case " + union_name + "::NONE:";
    code_ += "default:";
    code_.IncrementIdentLevel();
    code_ += "return FInstancedStruct();";
    code_.DecrementIdentLevel();
    code_ += "}";
    CloseBlock();
    code_ += "";

    code_ += "inline FInstancedStruct " + ue_name +
             "::" + UnionFromNativeName(union_def) + "(const " + union_native +
             "& InValue)";
    code_ += "{";
    code_.IncrementIdentLevel();
    code_ += "switch (InValue.type)";
    code_ += "{";
    for (const EnumVal* ev : members) {
      const std::string member_ue =
          UnrealStructName(*ev->union_type.struct_def);
      code_ += "case " + union_name + "::" + ev->name + ":";
      code_.IncrementIdentLevel();
      code_ += "return FInstancedStruct::Make<" + member_ue + ">(" +
               member_ue + "::FromNative(*InValue.As" + ev->name + "()));";
      code_.DecrementIdentLevel();
    }
    code_ += "case " + union_name + "::NONE:";
    code_ += "default:";
    code_.IncrementIdentLevel();
    code_ += "return FInstancedStruct();";
    code_.DecrementIdentLevel();
    code_ += "}";
    CloseBlock();
    code_ += "";

    code_ += "inline " + union_native + " " + ue_name +
             "::" + UnionToNativeName(union_def) +
             "(const FInstancedStruct& InValue)";
    code_ += "{";
    code_.IncrementIdentLevel();
    code_ += union_native + " Native;";
    for (const EnumVal* ev : members) {
      const std::string member_ue =
          UnrealStructName(*ev->union_type.struct_def);
      OpenBlock("if (const " + member_ue + "* Elem = InValue.GetPtr<" +
                member_ue + ">())");
      code_ += "Native.Set(Elem->ToNative());";
      code_ += "return Native;";
      CloseBlock();
    }
    code_ += "checkf(!InValue.IsValid(), TEXT(\"%s is not a member of " +
             union_name + "\"), *InValue.GetScriptStruct()->GetName());";
    code_ += "return Native;";
    CloseBlock();
    code_ += "";
  }

  void GenStructConversions(const StructDef& struct_def) {
    const std::string ue_name = UnrealStructName(struct_def);
    const std::string native_name = QualifiedNativeName(struct_def);
    const std::string fbs_name = QualifiedFbsName(struct_def);

    code_ += "inline " + native_name + " " + ue_name + "::ToNative() const";
    code_ += "{";
    code_.IncrementIdentLevel();
    if (struct_def.fixed) {
      std::string arguments;
      for (const FieldDef* field : struct_def.fields.vec) {
        if (SkipField(*field)) continue;
        if (!arguments.empty()) arguments += ", ";
        arguments += ToNativeValueExpr(field->value.type,
                                       UnrealFieldName(*field),
                                       FixedPointFractionBits(*field));
      }
      code_ += "return " + native_name + "(" + arguments + ");";
    } else {
      code_ += native_name + " Native;";
      for (const FieldDef* field : struct_def.fields.vec) {
        if (SkipField(*field)) continue;
        GenTableFieldToNative(struct_def, *field);
      }
      code_ += "return Native;";
    }
    CloseBlock();
    code_ += "";

    code_ += "inline " + ue_name + " " + ue_name + "::FromNative(const " +
             native_name + "& InNative)";
    code_ += "{";
    code_.IncrementIdentLevel();
    code_ += ue_name + " Result;";
    for (const FieldDef* field : struct_def.fields.vec) {
      if (SkipField(*field)) continue;
      if (struct_def.fixed) {
        code_ += "Result." + UnrealFieldName(*field) + " = " +
                 FromNativeValueExpr(field->value.type,
                                     "InNative." + field->name + "()",
                                     FixedPointFractionBits(*field)) +
                 ";";
      } else {
        GenTableFieldFromNative(struct_def, *field);
      }
    }
    code_ += "return Result;";
    CloseBlock();
    code_ += "";

    if (!struct_def.fixed) {
      code_ += "inline " + ue_name + " " + ue_name + "::FromTable(const " +
               fbs_name + "& InTable)";
      code_ += "{";
      code_.IncrementIdentLevel();
      code_ += ue_name + " Result;";
      for (const FieldDef* field : struct_def.fields.vec) {
        if (SkipField(*field)) continue;
        GenTableFieldFromTable(*field);
      }
      code_ += "return Result;";
      CloseBlock();
      code_ += "";
    }

    for (const EnumDef* union_def : UnionsOf(struct_def)) {
      GenUnionHelpers(struct_def, *union_def);
    }
  }

  const std::string unreal_module_;
  CodeWriter code_;
  TypedFloatConstantGenerator float_const_gen_;
  std::vector<const EnumDef*> marked_enums_;
  std::vector<const StructDef*> marked_structs_;
  std::vector<std::string> errors_;
  bool uses_enum_class_flags_ = false;
  bool uses_enum_range_ = false;
  bool uses_instanced_struct_ = false;
  bool uses_numeric_limits_ = false;
  bool uses_std_memory_ = false;
  bool uses_std_string_ = false;
};

}  // namespace cpp_unreal

namespace {

class CppUnrealCodeGenerator : public CodeGenerator {
 public:
  explicit CppUnrealCodeGenerator(const std::string& unreal_module)
      : unreal_module_(unreal_module) {}

  Status GenerateCode(const Parser& parser, const std::string& path,
                      const std::string& filename) override {
    if (!parser.opts.generate_object_based_api) {
      status_detail =
          ": --cpp-unreal requires --gen-object-api to also be set.";
      return Status::ERROR;
    }
    if (!cpp_unreal::CppScopedEnumsEnabled(parser.opts)) {
      status_detail =
          ": --cpp-unreal requires --scoped-enums or --cpp-std c++17 (or "
          "higher).";
      return Status::ERROR;
    }
    cpp_unreal::CppUnrealGenerator generator(parser, path, filename,
                                             unreal_module_);
    if (!generator.generate()) {
      status_detail = ":\n  " + generator.error_;
      return Status::ERROR;
    }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t*, int64_t, const CodeGenOptions&) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateMakeRule(const Parser& parser, const std::string& path,
                          const std::string& filename,
                          std::string& output) override {
    const std::string filebase = StripPath(StripExtension(filename));
    cpp_unreal::CppUnrealGenerator generator(parser, path, filebase,
                                             unreal_module_);
    const auto included_files = parser.GetIncludedFilesRecursive(filename);
    std::string make_rule =
        generator.GeneratedFileName(path, filebase + "_unreal", parser.opts) +
        ": ";
    for (const std::string& included_file : included_files) {
      make_rule += " " + included_file;
    }
    output = make_rule;
    return Status::OK;
  }

  Status GenerateGrpcCode(const Parser&, const std::string&,
                          const std::string&) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateRootFile(const Parser&, const std::string&) override {
    return Status::NOT_IMPLEMENTED;
  }

  bool IsSchemaOnly() const override { return true; }

  bool SupportsBfbsGeneration() const override { return false; }

  bool SupportsRootFileGeneration() const override { return false; }

  IDLOptions::Language Language() const override { return IDLOptions::kCpp; }

  std::string LanguageName() const override { return "C++ Unreal"; }

 private:
  const std::string unreal_module_;
};

}  // namespace

std::unique_ptr<CodeGenerator> NewCppUnrealCodeGenerator(
    const std::string& unreal_module) {
  return std::unique_ptr<CppUnrealCodeGenerator>(
      new CppUnrealCodeGenerator(unreal_module));
}

}  // namespace flatbuffers
