/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace {

// https://infra.spec.whatwg.org/#set-append
inline void append_to_ordered_set(Vector<DeprecatedString>& set, DeprecatedString item)
{
    if (!set.contains_slow(item))
        set.append(move(item));
}

// https://infra.spec.whatwg.org/#list-remove
inline void remove_from_ordered_set(Vector<DeprecatedString>& set, StringView item)
{
    set.remove_first_matching([&](auto const& value) { return value == item; });
}

// https://infra.spec.whatwg.org/#set-replace
inline void replace_in_ordered_set(Vector<DeprecatedString>& set, StringView item, DeprecatedString replacement)
{
    auto item_index = set.find_first_index(item);
    VERIFY(item_index.has_value());

    auto replacement_index = set.find_first_index(replacement);
    if (!replacement_index.has_value()) {
        set[*item_index] = move(replacement);
        return;
    }

    auto index_to_set = min(*item_index, *replacement_index);
    auto index_to_remove = max(*item_index, *replacement_index);
    if (index_to_set == index_to_remove)
        return;

    set[index_to_set] = move(replacement);
    set.remove(index_to_remove);
}

}

namespace Web::DOM {

WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMTokenList>> DOMTokenList::create(Element const& associated_element, DeprecatedFlyString associated_attribute)
{
    auto& realm = associated_element.realm();
    return MUST_OR_THROW_OOM(realm.heap().allocate<DOMTokenList>(realm, associated_element, move(associated_attribute)));
}

// https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A2
DOMTokenList::DOMTokenList(Element const& associated_element, DeprecatedFlyString associated_attribute)
    : Bindings::LegacyPlatformObject(associated_element.realm())
    , m_associated_element(associated_element)
    , m_associated_attribute(move(associated_attribute))
{
    auto value = associated_element.get_attribute(m_associated_attribute);
    associated_attribute_changed(value);
}

JS::ThrowCompletionOr<void> DOMTokenList::initialize(JS::Realm& realm)
{
    MUST_OR_THROW_OOM(Base::initialize(realm));
    set_prototype(&Bindings::ensure_web_prototype<Bindings::DOMTokenListPrototype>(realm, "DOMTokenList"));

    return {};
}

void DOMTokenList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_element);
}

// https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A1
void DOMTokenList::associated_attribute_changed(StringView value)
{
    m_token_set.clear();

    if (value.is_empty())
        return;

    auto split_values = value.split_view_if(Infra::is_ascii_whitespace);
    for (auto const& split_value : split_values)
        append_to_ordered_set(m_token_set, split_value);
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-indices%E2%91%A3
bool DOMTokenList::is_supported_property_index(u32 index) const
{
    return index < m_token_set.size();
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-item
DeprecatedString const& DOMTokenList::item(size_t index) const
{
    static const DeprecatedString null_string {};

    // 1. If index is equal to or greater than this’s token set’s size, then return null.
    if (index >= m_token_set.size())
        return null_string;

    // 2. Return this’s token set[index].
    return m_token_set[index];
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-contains
bool DOMTokenList::contains(StringView token)
{
    return m_token_set.contains_slow(token);
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-add
WebIDL::ExceptionOr<void> DOMTokenList::add(Vector<DeprecatedString> const& tokens)
{
    // 1. For each token in tokens:
    for (auto const& token : tokens) {
        // a. If token is the empty string, then throw a "SyntaxError" DOMException.
        // b. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
        TRY(validate_token(token));

        // 2. For each token in tokens, append token to this’s token set.
        append_to_ordered_set(m_token_set, token);
    }

    // 3. Run the update steps.
    run_update_steps();
    return {};
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-remove
WebIDL::ExceptionOr<void> DOMTokenList::remove(Vector<DeprecatedString> const& tokens)
{
    // 1. For each token in tokens:
    for (auto const& token : tokens) {
        // a. If token is the empty string, then throw a "SyntaxError" DOMException.
        // b. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
        TRY(validate_token(token));

        // 2. For each token in tokens, remove token from this’s token set.
        remove_from_ordered_set(m_token_set, token);
    }

    // 3. Run the update steps.
    run_update_steps();
    return {};
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-toggle
WebIDL::ExceptionOr<bool> DOMTokenList::toggle(DeprecatedString const& token, Optional<bool> force)
{
    // 1. If token is the empty string, then throw a "SyntaxError" DOMException.
    // 2. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
    TRY(validate_token(token));

    // 3. If this’s token set[token] exists, then:
    if (contains(token)) {
        // a. If force is either not given or is false, then remove token from this’s token set, run the update steps and return false.
        if (!force.has_value() || !force.value()) {
            remove_from_ordered_set(m_token_set, token);
            run_update_steps();
            return false;
        }

        // b. Return true.
        return true;
    }

    // 4. Otherwise, if force not given or is true, append token to this’s token set, run the update steps, and return true.
    if (!force.has_value() || force.value()) {
        append_to_ordered_set(m_token_set, token);
        run_update_steps();
        return true;
    }

    // 5. Return false.
    return false;
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-replace
WebIDL::ExceptionOr<bool> DOMTokenList::replace(DeprecatedString const& token, DeprecatedString const& new_token)
{
    // 1. If either token or newToken is the empty string, then throw a "SyntaxError" DOMException.
    // 2. If either token or newToken contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
    TRY(validate_token(token));
    TRY(validate_token(new_token));

    // 3. If this’s token set does not contain token, then return false.
    if (!contains(token))
        return false;

    // 4. Replace token in this’s token set with newToken.
    replace_in_ordered_set(m_token_set, token, new_token);

    // 5. Run the update steps.
    run_update_steps();

    // 6. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-supports
// https://dom.spec.whatwg.org/#concept-domtokenlist-validation
WebIDL::ExceptionOr<bool> DOMTokenList::supports([[maybe_unused]] StringView token)
{
    // FIXME: Implement this fully when any use case defines supported tokens.

    // 1. If the associated attribute’s local name does not define supported tokens, throw a TypeError.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, String::formatted("Attribute {} does not define any supported tokens", m_associated_attribute).release_value_but_fixme_should_propagate_errors() };

    // 2. Let lowercase token be a copy of token, in ASCII lowercase.
    // 3. If lowercase token is present in supported tokens, return true.
    // 4. Return false.
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-value
DeprecatedString DOMTokenList::value() const
{
    StringBuilder builder;
    builder.join(' ', m_token_set);
    return builder.to_deprecated_string();
}

// https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-set-value%E2%91%A2
void DOMTokenList::set_value(DeprecatedString value)
{
    JS::GCPtr<DOM::Element> associated_element = m_associated_element.ptr();
    if (!associated_element)
        return;

    MUST(associated_element->set_attribute(m_associated_attribute, move(value)));
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token(StringView token) const
{
    if (token.is_empty())
        return WebIDL::SyntaxError::create(realm(), "Non-empty DOM tokens are not allowed");
    if (any_of(token, Infra::is_ascii_whitespace))
        return WebIDL::InvalidCharacterError::create(realm(), "DOM tokens containing ASCII whitespace are not allowed");
    return {};
}

// https://dom.spec.whatwg.org/#concept-dtl-update
void DOMTokenList::run_update_steps()
{
    JS::GCPtr<DOM::Element> associated_element = m_associated_element.ptr();
    if (!associated_element)
        return;

    // 1. If the associated element does not have an associated attribute and token set is empty, then return.
    if (!associated_element->has_attribute(m_associated_attribute) && m_token_set.is_empty())
        return;

    // 2. Set an attribute value for the associated element using associated attribute’s local name and the result of running the ordered set serializer for token set.
    MUST(associated_element->set_attribute(m_associated_attribute, value()));
}

WebIDL::ExceptionOr<JS::Value> DOMTokenList::item_value(size_t index) const
{
    auto const& string = item(index);
    if (string.is_null())
        return JS::js_undefined();
    return JS::PrimitiveString::create(vm(), string);
}

}
