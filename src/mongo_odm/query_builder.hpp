// Copyright 2016 MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mongo_odm/config/prelude.hpp>

#include <cstddef>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/core.hpp>
#include <bsoncxx/view_or_value.hpp>

#include <bsoncxx/builder/stream/array.hpp>

#define NVP(x) mongo_odm::makeNvp(&mongo_odm_wrap_base::x, #x)

#define ODM_MAKE_KEYS(Base, ...)      \
    using mongo_odm_wrap_base = Base; \
    constexpr static auto fields = std::make_tuple(__VA_ARGS__);

#define ODM_MAKE_KEYS_STORAGE(Base) constexpr decltype(Base::fields) Base::fields

#define ODM_KEY(value) mongo_odm::hasCallIfFieldIsPresent<decltype(&value), &value>::call()

namespace mongo_odm {
MONGO_ODM_INLINE_NAMESPACE_BEGIN

/**
 * An object that represents a name-value pair of a member in an object.
 * It is templated on the class of the member and its type.
 */
template <typename Base, typename T>
struct Nvp {
    /**
     * Create a name-value pair from a member pointer and a name.
     * @param  t    A pointer to the member
     * @param  name The name of the member
     */
    constexpr Nvp(T Base::*t, const char *name) : t(t), name(name) {
    }

    T Base::*t;
    const char *name;
};

/* Create a name-value pair from a object member and its name */
template <typename Base, typename T>
Nvp<Base, T> constexpr makeNvp(T Base::*t, const char *name) {
    return Nvp<Base, T>(t, name);
}

/**
 * Represents a binary comparison expression between a key and a value. e.g. (User.age > 21).
 */
template <typename Base, typename T>
class ComparisonExpr {
   public:
    /**
     * Constructs a query expression for the given key, value, and comparison type
     * @param  nvp           A name-value pair corresponding to a key in a document
     * @param  field         The value that the key is being compared to.
     * @param  selector_type The type of comparison operator, such at gt (>) or ne (!=).
     */
    constexpr ComparisonExpr(const Nvp<Base, T> &nvp, T field, const char *selector_type)
        : nvp(nvp), field(std::move(field)), selector_type(selector_type) {
    }

    const Nvp<Base, T> &nvp;
    T field;
    const char *selector_type;

    /**
     * Appends this expression to a BSON core builder, as a key-value pair of the form
     * "key: {$cmp: val}", where $cmp is some comparison operator.
     * @param builder A BSON core builder
     */
    void append_to_bson(bsoncxx::builder::core &bson) const {
        bson.key_view(nvp.name);
        bson.open_document();
        bson.key_view(selector_type);
        bson.append(field);
        bson.close_document();
    }

    /**
     * Converts the expression to a BSON filter for a query.
     * The resulting BSON is of the form "{key: {$cmp: val}}".
     */
    operator bsoncxx::document::view_or_value() const {
        auto bson = bsoncxx::builder::core(false);
        append_to_bson(bson);
        return bsoncxx::document::view_or_value(bson.extract_document());
    }
};

/**
 * This represents an expression with the $not operator, which wraps a comparison expression and
 * negates it.
 */
template <typename Base, typename T>
class NotExpr {
   public:
    /**
     * Creates a $not expression taht negates the given comparison expression.
     * @param  expr A comparison expression
     */
    constexpr NotExpr(const ComparisonExpr<Base, T> &expr) : expr(expr) {
    }

    /**
     * Appends this expression to a BSON core builder,
     * as a key-value pair of the form "key: {$not: {$cmp: val}}".
     * @param builder a BSON core builder
     */
    void append_to_bson(bsoncxx::builder::core &bson) const {
        bson.key_view(expr.nvp.name);
        bson.open_document();
        bson.key_view("$not");
        bson.open_document();
        bson.key_view(expr.selector_type);
        bson.append(expr.field);
        bson.close_document();
        bson.close_document();
    }

    /**
     * Converts the expression to a BSON filter for a query.
     * The format of the BSON is "{key: {$not: {$cmp: val}}}".
     */
    operator bsoncxx::document::view_or_value() const {
        auto bson = bsoncxx::builder::core(false);
        append_to_bson(bson);
        return bsoncxx::document::view_or_value(bson.extract_document());
    }

    const ComparisonExpr<Base, T> expr;
};

template <typename Head, typename Tail>
class ExpressionList {
   public:
    /**
     * Constructs an expression list.
     * @param head  The first element in the list
     * @param tail  The remainder of the list
     */
    constexpr ExpressionList(const Head &head, const Tail &tail) : head(head), tail(tail) {
    }

    /**
     * Appends this expression list to the given core builder by appending the first expression
     * in the list, and then recursing on the rest of the list.
     * @param builder A basic BSON core builder.
     */
    void append_to_bson(bsoncxx::builder::core &bson) const {
        head.append_to_bson(bson);
        tail.append_to_bson(bson);
    }

    /**
     * Casts the expression list to a BSON query of  the form { expr1, expr2, ....}
     */
    operator bsoncxx::document::view_or_value() const {
        auto bson = bsoncxx::builder::core(false);
        append_to_bson(bson);
        return bsoncxx::document::view_or_value(bson.extract_document());
    }

    const Head head;
    const Tail tail;
};

template <typename Expr1, typename Expr2>
class BooleanExpr {
   public:
    /**
     * Constructs a boolean expression from two other expressions, and a certain operator.
     * @param  lhs The left-hand side of the expression.
     * @param  rhs The right-hand side of the expression.
     * @param  op  The operator of the expression, e.g. AND or OR.
     */
    constexpr BooleanExpr(const Expr1 &lhs, const Expr2 &rhs, const char *op)
        : lhs(lhs), rhs(rhs), op(op) {
    }

    /**
     * Appends this query to a BSON core builder as a key-value pair "$op: [{lhs}, {rhs}]"
     * @param builder A basic BSON core builder.
     */
    void append_to_bson(bsoncxx::builder::core &bson) const {
        bson.key_view(op);
        bson.open_array();

        // append left hand side
        bson.open_document();
        lhs.append_to_bson(bson);
        bson.close_document();

        // append right hand side
        bson.open_document();
        rhs.append_to_bson(bson);
        bson.close_document();

        bson.close_array();
    }

    /**
     * Converts the expression to a BSON filter for a query,
     * in the form "{ $op: [{lhs}, {rhs}] }"
     */
    operator bsoncxx::document::view_or_value() const {
        auto bson = bsoncxx::builder::core(false);
        append_to_bson(bson);
        return bsoncxx::document::view_or_value(bson.extract_document());
    }

    const Expr1 lhs;
    const Expr2 rhs;
    const char *op;
};

/* A templated struct that holds a boolean value.
* This value is TRUE for types that are query expression,
* and FALSE for all other types.
*/
template <typename>
struct is_expression {
    constexpr static bool value = false;
};

template <typename Base, typename T>
struct is_expression<ComparisonExpr<Base, T>> {
    constexpr static bool value = true;
};

template <typename Base, typename T>
struct is_expression<NotExpr<Base, T>> {
    constexpr static bool value = true;
};

template <typename Head, typename Tail>
struct is_expression<ExpressionList<Head, Tail>> {
    constexpr static bool value = true;
};

template <typename Expr1, typename Expr2>
struct is_expression<BooleanExpr<Expr1, Expr2>> {
    constexpr static bool value = true;
};

/* Operator overloads for creating and combining expressions */

/* Overload comparison operators for name-value pairs to create expressions.
 * Separate operators are defined for bool types to prevent confusing implicit casting to bool
 * for non-bool types.
 */
template <typename Base, typename T, typename U,
          typename = typename std::enable_if_t<!std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator==(const Nvp<Base, T> &lhs, const U &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$eq");
}

template <typename Base, typename T,
          typename = typename std::enable_if_t<std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator==(const Nvp<Base, T> &lhs, const T &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$eq");
}

template <typename Base, typename T, typename U,
          typename = typename std::enable_if_t<!std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator>(const Nvp<Base, T> &lhs, const U &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$gt");
}

template <typename Base, typename T,
          typename = typename std::enable_if_t<std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator>(const Nvp<Base, T> &lhs, const T &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$gt");
}

template <typename Base, typename T, typename U,
          typename = typename std::enable_if_t<!std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator>=(const Nvp<Base, T> &lhs, const U &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$gte");
}

template <typename Base, typename T,
          typename = typename std::enable_if_t<std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator>=(const Nvp<Base, T> &lhs, const T &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$gte");
}

template <typename Base, typename T, typename U,
          typename = typename std::enable_if_t<!std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator<(const Nvp<Base, T> &lhs, const U &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$lt");
}

template <typename Base, typename T,
          typename = typename std::enable_if_t<std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator<(const Nvp<Base, T> &lhs, const T &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$lt");
}

template <typename Base, typename T, typename U,
          typename = typename std::enable_if_t<!std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator<=(const Nvp<Base, T> &lhs, const U &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$lte");
}

template <typename Base, typename T,
          typename = typename std::enable_if_t<std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator<=(const Nvp<Base, T> &lhs, const T &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$lte");
}

template <typename Base, typename T, typename U,
          typename = typename std::enable_if_t<!std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator!=(const Nvp<Base, T> &lhs, const U &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$ne");
}

template <typename Base, typename T,
          typename = typename std::enable_if_t<std::is_same<T, bool>::value>>
constexpr ComparisonExpr<Base, T> operator!=(const Nvp<Base, T> &lhs, const T &rhs) {
    return ComparisonExpr<Base, T>(lhs, rhs, "$ne");
}

/**
 * Negates a comparison expression in a $not expression.
 */
template <typename Base, typename T>
constexpr NotExpr<Base, T> operator!(const ComparisonExpr<Base, T> &expr) {
    return NotExpr<Base, T>(expr);
}

/**
 * Comma operator that combines two expressions or an expression and an expression list into a new
 * expression list.
 */
template <typename Head, typename Tail,
          typename = typename std::enable_if<is_expression<Head>::value &&
                                             is_expression<Tail>::value>::type>
constexpr ExpressionList<Head, Tail> operator,(Tail &&tail, Head &&head) {
    return ExpressionList<Head, Tail>(head, tail);
}

/**
 * Boolean operator overloads for expressions.
 */
template <typename Expr1, typename Expr2,
          typename = typename std::enable_if<is_expression<Expr1>::value &&
                                             is_expression<Expr2>::value>::type>
constexpr BooleanExpr<Expr1, Expr2> operator&&(const Expr1 &lhs, const Expr2 &rhs) {
    return BooleanExpr<Expr1, Expr2>(lhs, rhs, "$and");
}

template <typename Expr1, typename Expr2,
          typename = typename std::enable_if<is_expression<Expr1>::value &&
                                             is_expression<Expr2>::value>::type>
constexpr BooleanExpr<Expr1, Expr2> operator||(const Expr1 &lhs, const Expr2 &rhs) {
    return BooleanExpr<Expr1, Expr2>(lhs, rhs, "$or");
}

/**
 * hasField determines whether a type Base has a member of the given type T as
 * the Nth member out of M total members which have name value pairs.
 */
// By default, if N>=M the index is out of bounds and hasField is false-y.
template <typename Base, typename T, size_t N, size_t M, bool = (N < M)>
struct hasField : public std::false_type {};

// Nth member in the Base::fields tuple (i.e. the list of fields for which we
// have name-value pairs)
// Must have same type as the given argument.
template <typename Base, typename T, size_t N, size_t M>
struct hasField<Base, T, N, M, true>
    : public std::is_same<T Base::*, decltype(std::get<N>(Base::fields).t)> {};

/**
 * wrapimpl uses template arguments N and M to iterate over the fields of a Base
 * class, and returns the name-value pair corresponding to the given member
 * field.
 */

// forward declarations for wrapimpl
template <typename Base, typename T, size_t N, size_t M>
constexpr std::enable_if_t<N == M, const Nvp<Base, T> *> wrapimpl(T Base::*t);

template <typename Base, typename T, size_t N, size_t M>
constexpr std::enable_if_t<(N < M) && !hasField<Base, T, N, M>::value, const Nvp<Base, T> *>
wrapimpl(T Base::*t);

// If Nth field has same type as T, check that it points to the same member.
// If not, check (N+1)th field.
template <typename Base, typename T, size_t N, size_t M>
constexpr std::enable_if_t<(N < M) && hasField<Base, T, N, M>::value, const Nvp<Base, T> *>
wrapimpl(T Base::*t) {
    if (std::get<N>(Base::fields).t == t) {
        return &std::get<N>(Base::fields);
    } else {
        return wrapimpl<Base, T, N + 1, M>(t);
    }
}

// If current field doesn't match the type of T, check (N+1)th field.
template <typename Base, typename T, size_t N, size_t M>
constexpr std::enable_if_t<(N < M) && !hasField<Base, T, N, M>::value, const Nvp<Base, T> *>
wrapimpl(T Base::*t) {
    return wrapimpl<Base, T, N + 1, M>(t);
}

// If N==M, we've gone past the last field, return nullptr.
template <typename Base, typename T, size_t N, size_t M>
constexpr std::enable_if_t<N == M, const Nvp<Base, T> *> wrapimpl(T Base::*t) {
    return nullptr;
}

/**
 * Returns a name-value pair corresponding to the given member pointer.
 * @tparam Base The class containing the member in question
 * @tparam T    The type of the member
 * @param t     A member pointer to an element of type T in the class Base.
 * @return      The name-value pair corresponding to this member pointer.
 */
template <typename Base, typename T>
constexpr const Nvp<Base, T> *wrap(T Base::*t) {
    return wrapimpl<Base, T, 0, std::tuple_size<decltype(Base::fields)>::value>(t);
}

/**
 * A struct that has a call() method that returns a name-value pair corresponding
 * to the given member pointer,
 * but only if such a member exists.
 */
template <typename T, T, typename = void>
struct hasCallIfFieldIsPresent {};

template <typename Base, typename T, T Base::*ptr>
struct hasCallIfFieldIsPresent<T Base::*, ptr, std::enable_if_t<wrap(ptr) != nullptr>> {
    static constexpr const Nvp<Base, T> &call() {
        return *wrap(ptr);
    }
};

MONGO_ODM_INLINE_NAMESPACE_END
}  // namespace bson_mapper

#include <mongo_odm/config/postlude.hpp>
