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
#include <boost/algorithm/string/replace.hpp>
#include <sstream>
#include <utility>

#include "Compiler.hh"
#include "Schema.hh"
#include "Stream.hh"
#include "Types.hh"
#include "ValidSchema.hh"

#include "json/JsonDom.hh"

using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::vector;

namespace avro {
using json::Array;
using json::Entity;
using json::EntityType;
using json::Object;

using SymbolTable = map<Name, NodePtr>;

// #define DEBUG_VERBOSE

static NodePtr makePrimitive(const string &t) {
    if (t == "null") {
        return NodePtr(new NodePrimitive(AVRO_NULL));
    } else if (t == "boolean") {
        return NodePtr(new NodePrimitive(AVRO_BOOL));
    } else if (t == "int") {
        return NodePtr(new NodePrimitive(AVRO_INT));
    } else if (t == "long") {
        return NodePtr(new NodePrimitive(AVRO_LONG));
    } else if (t == "float") {
        return NodePtr(new NodePrimitive(AVRO_FLOAT));
    } else if (t == "double") {
        return NodePtr(new NodePrimitive(AVRO_DOUBLE));
    } else if (t == "string") {
        return NodePtr(new NodePrimitive(AVRO_STRING));
    } else if (t == "bytes") {
        return NodePtr(new NodePrimitive(AVRO_BYTES));
    } else {
        return NodePtr();
    }
}

static NodePtr makeNode(const json::Entity &e, SymbolTable &st, const string &ns);

template<typename T>
concepts::SingleAttribute<T> asSingleAttribute(const T &t) {
    concepts::SingleAttribute<T> n;
    n.add(t);
    return n;
}

static bool isFullName(const string &s) {
    return s.find('.') != string::npos;
}

static Name getName(const string &name, const string &ns) {
    return (isFullName(name)) ? Name(name) : Name(name, ns);
}

static NodePtr makeNode(const string &t, SymbolTable &st, const string &ns) {
    NodePtr result = makePrimitive(t);
    if (result) {
        return result;
    }
    Name n = getName(t, ns);

    auto it = st.find(n);
    if (it != st.end()) {
        return NodePtr(new NodeSymbolic(asSingleAttribute(n), it->second));
    }
    throw Exception(boost::format("Unknown type: %1%") % n.fullname());
}

/** Returns "true" if the field is in the container */
// e.g.: can be false for non-mandatory fields
bool containsField(const Object &m, const string &fieldName) {
    auto it = m.find(fieldName);
    return (it != m.end());
}

json::Object::const_iterator findField(const Entity &e,
                                       const Object &m, const string &fieldName);

template<typename T>
void ensureType(const Entity &e, const string &name) {
    if (e.type() != json::type_traits<T>::type()) {
        throw Exception(boost::format("Json field \"%1%\" is not a %2%: %3%") % name % json::type_traits<T>::name() % e.toString());
    }
}

string getStringField(const Entity &e, const Object &m,
                      const string &fieldName) {
    auto it = findField(e, m, fieldName);
    ensureType<string>(it->second, fieldName);
    return it->second.stringValue();
}

const Array &getArrayField(const Entity &e, const Object &m,
                           const string &fieldName);

int64_t getLongField(const Entity &e, const Object &m,
                     const string &fieldName) {
    auto it = findField(e, m, fieldName);
    ensureType<int64_t>(it->second, fieldName);
    return it->second.longValue();
}

// Unescape double quotes (") for de-serialization.  This method complements the
// method NodeImpl::escape() which is used for serialization.
static void unescape(string &s) {
    boost::replace_all(s, "\\\"", "\"");
}

string getDocField(const Entity &e, const Object &m) {
    string doc = getStringField(e, m, "doc");
    unescape(doc);
    return doc;
}

struct Field {
    const string name;
    const NodePtr schema;
    const GenericDatum defaultValue;
    Field(string n, NodePtr v, GenericDatum dv) : name(std::move(n)), schema(std::move(v)), defaultValue(std::move(dv)) {}
};

static void assertType(const Entity &e, EntityType et) {
    if (e.type() != et) {
        throw Exception(boost::format("Unexpected type for default value: "
                                      "Expected %1%, but found %2% in line %3%")
                        % json::typeToString(et) % json::typeToString(e.type()) % e.line());
    }
}

static vector<uint8_t> toBin(const string &s) {
    vector<uint8_t> result(s.size());
    if (!s.empty()) {
        std::copy(s.c_str(), s.c_str() + s.size(), result.data());
    }
    return result;
}

static GenericDatum makeGenericDatum(NodePtr n,
                                     const Entity &e, const SymbolTable &st) {
    Type t = n->type();
    EntityType dt = e.type();

    if (t == AVRO_SYMBOLIC) {
        n = st.find(n->name())->second;
        t = n->type();
    }
    switch (t) {
        case AVRO_STRING:
            assertType(e, json::etString);
            return GenericDatum(e.stringValue());
        case AVRO_BYTES:
            assertType(e, json::etString);
            return GenericDatum(toBin(e.bytesValue()));
        case AVRO_INT:
            assertType(e, json::etLong);
            return GenericDatum(static_cast<int32_t>(e.longValue()));
        case AVRO_LONG:
            assertType(e, json::etLong);
            return GenericDatum(e.longValue());
        case AVRO_FLOAT:
            if (dt == json::etLong) {
                return GenericDatum(static_cast<float>(e.longValue()));
            }
            assertType(e, json::etDouble);
            return GenericDatum(static_cast<float>(e.doubleValue()));
        case AVRO_DOUBLE:
            if (dt == json::etLong) {
                return GenericDatum(static_cast<double>(e.longValue()));
            }
            assertType(e, json::etDouble);
            return GenericDatum(e.doubleValue());
        case AVRO_BOOL:
            assertType(e, json::etBool);
            return GenericDatum(e.boolValue());
        case AVRO_NULL:
            assertType(e, json::etNull);
            return GenericDatum();
        case AVRO_RECORD: {
            assertType(e, json::etObject);
            GenericRecord result(n);
            const map<string, Entity> &v = e.objectValue();
            for (size_t i = 0; i < n->leaves(); ++i) {
                auto it = v.find(n->nameAt(i));
                if (it == v.end()) {
                    throw Exception(boost::format(
                                        "No value found in default for %1%")
                                    % n->nameAt(i));
                }
                result.setFieldAt(i,
                                  makeGenericDatum(n->leafAt(i), it->second, st));
            }
            return GenericDatum(n, result);
        }
        case AVRO_ENUM:
            assertType(e, json::etString);
            return GenericDatum(n, GenericEnum(n, e.stringValue()));
        case AVRO_ARRAY: {
            assertType(e, json::etArray);
            GenericArray result(n);
            const vector<Entity> &elements = e.arrayValue();
            for (const auto &element : elements) {
                result.value().push_back(makeGenericDatum(n->leafAt(0), element, st));
            }
            return GenericDatum(n, result);
        }
        case AVRO_MAP: {
            assertType(e, json::etObject);
            GenericMap result(n);
            const map<string, Entity> &v = e.objectValue();
            for (const auto &it : v) {
                result.value().push_back(make_pair(it.first,
                                                   makeGenericDatum(n->leafAt(1), it.second, st)));
            }
            return GenericDatum(n, result);
        }
        case AVRO_UNION: {
            GenericUnion result(n);
            result.selectBranch(0);
            result.datum() = makeGenericDatum(n->leafAt(0), e, st);
            return GenericDatum(n, result);
        }
        case AVRO_FIXED:
            assertType(e, json::etString);
            return GenericDatum(n, GenericFixed(n, toBin(e.bytesValue())));
        default: throw Exception(boost::format("Unknown type: %1%") % t);
    }
}

static Field makeField(const Entity &e, SymbolTable &st, const string &ns) {
    const Object &m = e.objectValue();
    const string &n = getStringField(e, m, "name");
    auto it = findField(e, m, "type");
    auto it2 = m.find("default");
    NodePtr node = makeNode(it->second, st, ns);
    if (containsField(m, "doc")) {
        node->setDoc(getDocField(e, m));
    }
    GenericDatum d = (it2 == m.end()) ? GenericDatum() : makeGenericDatum(node, it2->second, st);
    return Field(n, node, d);
}

// Extended makeRecordNode (with doc).
static NodePtr makeRecordNode(const Entity &e, const Name &name,
                              const string *doc, const Object &m,
                              SymbolTable &st, const string &ns) {
    const Array &v = getArrayField(e, m, "fields");
    concepts::MultiAttribute<string> fieldNames;
    concepts::MultiAttribute<NodePtr> fieldValues;
    vector<GenericDatum> defaultValues;

    for (const auto &it : v) {
        Field f = makeField(it, st, ns);
        fieldNames.add(f.name);
        fieldValues.add(f.schema);
        defaultValues.push_back(f.defaultValue);
    }
    NodeRecord *node;
    if (doc == nullptr) {
        node = new NodeRecord(asSingleAttribute(name), fieldValues, fieldNames,
                              defaultValues);
    } else {
        node = new NodeRecord(asSingleAttribute(name), asSingleAttribute(*doc),
                              fieldValues, fieldNames, defaultValues);
    }
    return NodePtr(node);
}

static LogicalType makeLogicalType(const Entity &e, const Object &m) {
    if (!containsField(m, "logicalType")) {
        return LogicalType(LogicalType::NONE);
    }

    const std::string &typeField = getStringField(e, m, "logicalType");

    if (typeField == "decimal") {
        LogicalType decimalType(LogicalType::DECIMAL);
        try {
            decimalType.setPrecision(getLongField(e, m, "precision"));
            if (containsField(m, "scale")) {
                decimalType.setScale(getLongField(e, m, "scale"));
            }
        } catch (Exception &ex) {
            // If any part of the logical type is malformed, per the standard we
            // must ignore the whole attribute.
            return LogicalType(LogicalType::NONE);
        }
        return decimalType;
    }

    LogicalType::Type t = LogicalType::NONE;
    if (typeField == "date")
        t = LogicalType::DATE;
    else if (typeField == "time-millis")
        t = LogicalType::TIME_MILLIS;
    else if (typeField == "time-micros")
        t = LogicalType::TIME_MICROS;
    else if (typeField == "timestamp-millis")
        t = LogicalType::TIMESTAMP_MILLIS;
    else if (typeField == "timestamp-micros")
        t = LogicalType::TIMESTAMP_MICROS;
    else if (typeField == "duration")
        t = LogicalType::DURATION;
    else if (typeField == "uuid")
        t = LogicalType::UUID;
    return LogicalType(t);
}

static NodePtr makeEnumNode(const Entity &e,
                            const Name &name, const Object &m) {
    const Array &v = getArrayField(e, m, "symbols");
    concepts::MultiAttribute<string> symbols;
    for (const auto &it : v) {
        if (it.type() != json::etString) {
            throw Exception(boost::format("Enum symbol not a string: %1%") % it.toString());
        }
        symbols.add(it.stringValue());
    }
    NodePtr node = NodePtr(new NodeEnum(asSingleAttribute(name), symbols));
    if (containsField(m, "doc")) {
        node->setDoc(getDocField(e, m));
    }
    return node;
}

static NodePtr makeFixedNode(const Entity &e,
                             const Name &name, const Object &m) {
    int v = static_cast<int>(getLongField(e, m, "size"));
    if (v <= 0) {
        throw Exception(boost::format("Size for fixed is not positive: %1%") % e.toString());
    }
    NodePtr node =
        NodePtr(new NodeFixed(asSingleAttribute(name), asSingleAttribute(v)));
    if (containsField(m, "doc")) {
        node->setDoc(getDocField(e, m));
    }
    return node;
}

static NodePtr makeArrayNode(const Entity &e, const Object &m,
                             SymbolTable &st, const string &ns) {
    auto it = findField(e, m, "items");
    NodePtr node = NodePtr(new NodeArray(
        asSingleAttribute(makeNode(it->second, st, ns))));
    if (containsField(m, "doc")) {
        node->setDoc(getDocField(e, m));
    }
    return node;
}

static NodePtr makeMapNode(const Entity &e, const Object &m,
                           SymbolTable &st, const string &ns) {
    auto it = findField(e, m, "values");

    NodePtr node = NodePtr(new NodeMap(
        asSingleAttribute(makeNode(it->second, st, ns))));
    if (containsField(m, "doc")) {
        node->setDoc(getDocField(e, m));
    }
    return node;
}

static Name getName(const Entity &e, const Object &m, const string &ns) {
    const string &name = getStringField(e, m, "name");

    if (isFullName(name)) {
        return Name(name);
    } else {
        auto it = m.find("namespace");
        if (it != m.end()) {
            if (it->second.type() != json::type_traits<string>::type()) {
                throw Exception(boost::format(
                                    "Json field \"%1%\" is not a %2%: %3%")
                                % "namespace" % json::type_traits<string>::name() % it->second.toString());
            }
            Name result = Name(name, it->second.stringValue());
            return result;
        }
        return Name(name, ns);
    }
}

static NodePtr makeNode(const Entity &e, const Object &m,
                        SymbolTable &st, const string &ns) {
    const string &type = getStringField(e, m, "type");
    NodePtr result;
    if (type == "record" || type == "error" || type == "enum" || type == "fixed") {
        Name nm = getName(e, m, ns);
        if (type == "record" || type == "error") {
            result = NodePtr(new NodeRecord());
            st[nm] = result;
            // Get field doc
            if (containsField(m, "doc")) {
                string doc = getDocField(e, m);

                NodePtr r = makeRecordNode(e, nm, &doc, m, st, nm.ns());
                (std::dynamic_pointer_cast<NodeRecord>(r))->swap(*std::dynamic_pointer_cast<NodeRecord>(result));
            } else { // No doc
                NodePtr r =
                    makeRecordNode(e, nm, nullptr, m, st, nm.ns());
                (std::dynamic_pointer_cast<NodeRecord>(r))
                    ->swap(*std::dynamic_pointer_cast<NodeRecord>(result));
            }
        } else {
            result = (type == "enum") ? makeEnumNode(e, nm, m) : makeFixedNode(e, nm, m);
            st[nm] = result;
        }
    } else if (type == "array") {
        result = makeArrayNode(e, m, st, ns);
    } else if (type == "map") {
        result = makeMapNode(e, m, st, ns);
    } else {
        result = makePrimitive(type);
    }

    if (result) {
        try {
            result->setLogicalType(makeLogicalType(e, m));
        } catch (Exception &ex) {
            // Per the standard we must ignore the logical type attribute if it
            // is malformed.
        }
        return result;
    }

    throw Exception(boost::format("Unknown type definition: %1%")
                    % e.toString());
}

static NodePtr makeNode(const Entity &e, const Array &m,
                        SymbolTable &st, const string &ns) {
    concepts::MultiAttribute<NodePtr> mm;
    for (const auto &it : m) {
        mm.add(makeNode(it, st, ns));
    }
    return NodePtr(new NodeUnion(mm));
}

static NodePtr makeNode(const json::Entity &e, SymbolTable &st, const string &ns) {
    switch (e.type()) {
        case json::etString: return makeNode(e.stringValue(), st, ns);
        case json::etObject: return makeNode(e, e.objectValue(), st, ns);
        case json::etArray: return makeNode(e, e.arrayValue(), st, ns);
        default: throw Exception(boost::format("Invalid Avro type: %1%") % e.toString());
    }
}
json::Object::const_iterator findField(const Entity &e, const Object &m, const string &fieldName) {
    auto it = m.find(fieldName);
    if (it == m.end()) {
        throw Exception(boost::format("Missing Json field \"%1%\": %2%") % fieldName % e.toString());
    } else {
        return it;
    }
}
const Array &getArrayField(const Entity &e, const Object &m, const string &fieldName) {
    auto it = findField(e, m, fieldName);
    ensureType<Array>(it->second, fieldName);
    return it->second.arrayValue();
}

ValidSchema compileJsonSchemaFromStream(InputStream &is) {
    json::Entity e = json::loadEntity(is);
    SymbolTable st;
    NodePtr n = makeNode(e, st, "");
    return ValidSchema(n);
}

AVRO_DECL ValidSchema compileJsonSchemaFromFile(const char *filename) {
    std::unique_ptr<InputStream> s = fileInputStream(filename);
    return compileJsonSchemaFromStream(*s);
}

AVRO_DECL ValidSchema compileJsonSchemaFromMemory(const uint8_t *input, size_t len) {
    return compileJsonSchemaFromStream(*memoryInputStream(input, len));
}

AVRO_DECL ValidSchema compileJsonSchemaFromString(const char *input) {
    return compileJsonSchemaFromMemory(reinterpret_cast<const uint8_t *>(input),
                                       ::strlen(input));
}

AVRO_DECL ValidSchema compileJsonSchemaFromString(const string &input) {
    return compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t *>(input.data()), input.size());
}

static ValidSchema compile(std::istream &is) {
    std::unique_ptr<InputStream> in = istreamInputStream(is);
    return compileJsonSchemaFromStream(*in);
}

void compileJsonSchema(std::istream &is, ValidSchema &schema) {
    if (!is.good()) {
        throw Exception("Input stream is not good");
    }

    schema = compile(is);
}

AVRO_DECL bool compileJsonSchema(std::istream &is, ValidSchema &schema, string &error) {
    try {
        compileJsonSchema(is, schema);
        return true;
    } catch (const Exception &e) {
        error = e.what();
        return false;
    }
}

} // namespace avro
