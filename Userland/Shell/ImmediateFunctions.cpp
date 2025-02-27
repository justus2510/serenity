/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Formatter.h"
#include "Shell.h"
#include <LibRegex/Regex.h>

namespace Shell {

ErrorOr<RefPtr<AST::Node>> Shell::immediate_length_impl(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments, bool across)
{
    auto name = across ? "length_across" : "length";
    if (arguments.size() < 1 || arguments.size() > 2) {
        raise_error(ShellError::EvaluatedSyntaxError, DeprecatedString::formatted("Expected one or two arguments to `{}'", name), invoking_node.position());
        return nullptr;
    }

    enum {
        Infer,
        String,
        List,
    } mode { Infer };

    bool is_inferred = false;

    const AST::Node* expr_node;
    if (arguments.size() == 2) {
        // length string <expr>
        // length list <expr>

        auto& mode_arg = arguments.first();
        if (!mode_arg.is_bareword()) {
            raise_error(ShellError::EvaluatedSyntaxError, DeprecatedString::formatted("Expected a bareword (either 'string' or 'list') in the two-argument form of the `{}' immediate", name), mode_arg.position());
            return nullptr;
        }

        auto const& mode_name = static_cast<const AST::BarewordLiteral&>(mode_arg).text();
        if (mode_name == "list") {
            mode = List;
        } else if (mode_name == "string") {
            mode = String;
        } else if (mode_name == "infer") {
            mode = Infer;
        } else {
            raise_error(ShellError::EvaluatedSyntaxError, DeprecatedString::formatted("Expected either 'string' or 'list' (and not {}) in the two-argument form of the `{}' immediate", mode_name, name), mode_arg.position());
            return nullptr;
        }

        expr_node = &arguments[1];
    } else {
        expr_node = &arguments[0];
    }

    if (mode == Infer) {
        is_inferred = true;
        if (expr_node->is_list())
            mode = List;
        else if (expr_node->is_simple_variable()) // "Look inside" variables
            mode = TRY(TRY(const_cast<AST::Node*>(expr_node)->run(this))->resolve_without_cast(this))->is_list_without_resolution() ? List : String;
        else if (is<AST::ImmediateExpression>(expr_node))
            mode = List;
        else
            mode = String;
    }

    auto value_with_number = [&](auto number) -> ErrorOr<NonnullRefPtr<AST::Node>> {
        return AST::make_ref_counted<AST::BarewordLiteral>(invoking_node.position(), TRY(String::number(number)));
    };

    auto do_across = [&](StringView mode_name, auto& values) -> ErrorOr<RefPtr<AST::Node>> {
        if (is_inferred)
            mode_name = "infer"sv;
        // Translate to a list of applications of `length <mode_name>`
        Vector<NonnullRefPtr<AST::Node>> resulting_nodes;
        resulting_nodes.ensure_capacity(values.size());
        for (auto& entry : values) {
            // ImmediateExpression(length <mode_name> <entry>)
            resulting_nodes.unchecked_append(AST::make_ref_counted<AST::ImmediateExpression>(
                expr_node->position(),
                AST::NameWithPosition { TRY("length"_string), invoking_node.function_position() },
                NonnullRefPtrVector<AST::Node> { Vector<NonnullRefPtr<AST::Node>> {
                    static_cast<NonnullRefPtr<AST::Node>>(AST::make_ref_counted<AST::BarewordLiteral>(expr_node->position(), TRY(String::from_utf8(mode_name)))),
                    AST::make_ref_counted<AST::SyntheticNode>(expr_node->position(), NonnullRefPtr<AST::Value>(entry)),
                } },
                expr_node->position()));
        }

        return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), move(resulting_nodes));
    };

    switch (mode) {
    default:
    case Infer:
        VERIFY_NOT_REACHED();
    case List: {
        auto value = TRY(const_cast<AST::Node*>(expr_node)->run(this));
        if (!value)
            return value_with_number(0);

        value = TRY(value->resolve_without_cast(this));

        if (auto list = dynamic_cast<AST::ListValue*>(value.ptr())) {
            if (across)
                return do_across("list"sv, list->values());

            return value_with_number(list->values().size());
        }

        auto list = TRY(value->resolve_as_list(this));
        if (!across)
            return value_with_number(list.size());

        dbgln("List has {} entries", list.size());
        auto values = AST::make_ref_counted<AST::ListValue>(move(list));
        return do_across("list"sv, values->values());
    }
    case String: {
        // 'across' will only accept lists, and '!across' will only accept non-lists here.
        if (expr_node->is_list()) {
            if (!across) {
            raise_no_list_allowed:;
                Formatter formatter { *expr_node };

                if (is_inferred) {
                    raise_error(ShellError::EvaluatedSyntaxError,
                        DeprecatedString::formatted("Could not infer expression type, please explicitly use `{0} string' or `{0} list'", name),
                        invoking_node.position());
                    return nullptr;
                }

                auto source = formatter.format();
                raise_error(ShellError::EvaluatedSyntaxError,
                    source.is_empty()
                        ? "Invalid application of `length' to a list"
                        : DeprecatedString::formatted("Invalid application of `length' to a list\nperhaps you meant `{1}length \"{0}\"{2}' or `{1}length_across {0}{2}'?", source, "\x1b[32m", "\x1b[0m"),
                    expr_node->position());
                return nullptr;
            }
        }

        auto value = TRY(const_cast<AST::Node*>(expr_node)->run(this));
        if (!value)
            return value_with_number(0);

        value = TRY(value->resolve_without_cast(*this));

        if (auto list = dynamic_cast<AST::ListValue*>(value.ptr())) {
            if (!across)
                goto raise_no_list_allowed;

            return do_across("string"sv, list->values());
        }

        if (across && !value->is_list()) {
            Formatter formatter { *expr_node };

            auto source = formatter.format();
            raise_error(ShellError::EvaluatedSyntaxError,
                DeprecatedString::formatted("Invalid application of `length_across' to a non-list\nperhaps you meant `{1}length {0}{2}'?", source, "\x1b[32m", "\x1b[0m"),
                expr_node->position());
            return nullptr;
        }

        // Evaluate the nodes and substitute with the lengths.
        auto list = TRY(value->resolve_as_list(this));

        if (!expr_node->is_list()) {
            if (list.size() == 1) {
                if (across)
                    goto raise_no_list_allowed;

                // This is the normal case, the expression is a normal non-list expression.
                return value_with_number(list.first().bytes_as_string_view().length());
            }

            // This can be hit by asking for the length of a command list (e.g. `(>/dev/null)`)
            // raise an error about misuse of command lists for now.
            // FIXME: What's the length of `(>/dev/null)` supposed to be?
            raise_error(ShellError::EvaluatedSyntaxError, "Length of meta value (or command list) requested, this is currently not supported.", expr_node->position());
            return nullptr;
        }

        auto values = AST::make_ref_counted<AST::ListValue>(move(list));
        return do_across("string"sv, values->values());
    }
    }
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_length(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    return immediate_length_impl(invoking_node, arguments, false);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_length_across(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    return immediate_length_impl(invoking_node, arguments, true);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_regex_replace(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 3) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 3 arguments to regex_replace", invoking_node.position());
        return nullptr;
    }

    auto pattern = TRY(const_cast<AST::Node&>(arguments[0]).run(this));
    auto replacement = TRY(const_cast<AST::Node&>(arguments[1]).run(this));
    auto value = TRY(TRY(const_cast<AST::Node&>(arguments[2]).run(this))->resolve_without_cast(this));

    if (!pattern->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the regex_replace pattern to be a string", arguments[0].position());
        return nullptr;
    }

    if (!replacement->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the regex_replace replacement string to be a string", arguments[1].position());
        return nullptr;
    }

    if (!value->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the regex_replace target value to be a string", arguments[2].position());
        return nullptr;
    }

    Regex<PosixExtendedParser> re { TRY(pattern->resolve_as_list(this)).first().to_deprecated_string() };
    auto result = re.replace(
        TRY(value->resolve_as_list(this))[0],
        TRY(replacement->resolve_as_list(this))[0],
        PosixFlags::Global | PosixFlags::Multiline | PosixFlags::Unicode);

    return AST::make_ref_counted<AST::StringLiteral>(invoking_node.position(), TRY(String::from_utf8(result)), AST::StringLiteral::EnclosureType::None);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_remove_suffix(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to remove_suffix", invoking_node.position());
        return nullptr;
    }

    auto suffix = TRY(const_cast<AST::Node&>(arguments[0]).run(this));
    auto value = TRY(TRY(const_cast<AST::Node&>(arguments[1]).run(this))->resolve_without_cast(this));

    if (!suffix->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the remove_suffix suffix string to be a string", arguments[0].position());
        return nullptr;
    }

    auto suffix_str = TRY(suffix->resolve_as_list(this))[0];
    auto values = TRY(value->resolve_as_list(this));

    Vector<NonnullRefPtr<AST::Node>> nodes;

    for (auto& value_str : values) {
        String removed = TRY(String::from_utf8(value_str));

        if (value_str.bytes_as_string_view().ends_with(suffix_str))
            removed = TRY(removed.substring_from_byte_offset(0, value_str.bytes_as_string_view().length() - suffix_str.bytes_as_string_view().length()));

        nodes.append(AST::make_ref_counted<AST::StringLiteral>(invoking_node.position(), move(removed), AST::StringLiteral::EnclosureType::None));
    }

    return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), move(nodes));
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_remove_prefix(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to remove_prefix", invoking_node.position());
        return nullptr;
    }

    auto prefix = TRY(const_cast<AST::Node&>(arguments[0]).run(this));
    auto value = TRY(TRY(const_cast<AST::Node&>(arguments[1]).run(this))->resolve_without_cast(this));

    if (!prefix->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the remove_prefix prefix string to be a string", arguments[0].position());
        return nullptr;
    }

    auto prefix_str = TRY(prefix->resolve_as_list(this))[0];
    auto values = TRY(value->resolve_as_list(this));

    Vector<NonnullRefPtr<AST::Node>> nodes;

    for (auto& value_str : values) {
        String removed = TRY(String::from_utf8(value_str));

        if (value_str.bytes_as_string_view().starts_with(prefix_str))
            removed = TRY(removed.substring_from_byte_offset(prefix_str.bytes_as_string_view().length()));
        nodes.append(AST::make_ref_counted<AST::StringLiteral>(invoking_node.position(), move(removed), AST::StringLiteral::EnclosureType::None));
    }

    return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), move(nodes));
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_split(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to split", invoking_node.position());
        return nullptr;
    }

    auto delimiter = TRY(const_cast<AST::Node&>(arguments[0]).run(this));
    auto value = TRY(TRY(const_cast<AST::Node&>(arguments[1]).run(this))->resolve_without_cast(this));

    if (!delimiter->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the split delimiter string to be a string", arguments[0].position());
        return nullptr;
    }

    auto delimiter_str = TRY(delimiter->resolve_as_list(this))[0];

    auto transform = [&](auto const& values) {
        // Translate to a list of applications of `split <delimiter>`
        Vector<NonnullRefPtr<AST::Node>> resulting_nodes;
        resulting_nodes.ensure_capacity(values.size());
        for (auto& entry : values) {
            // ImmediateExpression(split <delimiter> <entry>)
            resulting_nodes.unchecked_append(AST::make_ref_counted<AST::ImmediateExpression>(
                arguments[1].position(),
                invoking_node.function(),
                NonnullRefPtrVector<AST::Node> { Vector<NonnullRefPtr<AST::Node>> {
                    arguments[0],
                    AST::make_ref_counted<AST::SyntheticNode>(arguments[1].position(), NonnullRefPtr<AST::Value>(entry)),
                } },
                arguments[1].position()));
        }

        return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), move(resulting_nodes));
    };

    if (auto list = dynamic_cast<AST::ListValue*>(value.ptr())) {
        return transform(list->values());
    }

    // Otherwise, just resolve to a list and transform that.
    auto list = TRY(value->resolve_as_list(this));
    if (!value->is_list()) {
        if (list.is_empty())
            return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), NonnullRefPtrVector<AST::Node> {});

        auto& value = list.first();
        Vector<String> split_strings;
        if (delimiter_str.is_empty()) {
            StringBuilder builder;
            for (auto code_point : Utf8View { value }) {
                builder.append_code_point(code_point);
                split_strings.append(TRY(builder.to_string()));
                builder.clear();
            }
        } else {
            auto split = StringView { value }.split_view(delimiter_str, options.inline_exec_keep_empty_segments ? SplitBehavior::KeepEmpty : SplitBehavior::Nothing);
            split_strings.ensure_capacity(split.size());
            for (auto& entry : split)
                split_strings.append(TRY(String::from_utf8(entry)));
        }
        return AST::make_ref_counted<AST::SyntheticNode>(invoking_node.position(), AST::make_ref_counted<AST::ListValue>(move(split_strings)));
    }

    return transform(AST::make_ref_counted<AST::ListValue>(list)->values());
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_concat_lists(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    NonnullRefPtrVector<AST::Node> result;

    for (auto& argument : arguments) {
        if (auto* list = dynamic_cast<const AST::ListConcatenate*>(&argument)) {
            result.extend(list->list());
        } else {
            auto list_of_values = TRY(TRY(const_cast<AST::Node&>(argument).run(this))->resolve_without_cast(this));
            if (auto* list = dynamic_cast<AST::ListValue*>(list_of_values.ptr())) {
                for (auto& entry : static_cast<Vector<NonnullRefPtr<AST::Value>>&>(list->values()))
                    result.append(AST::make_ref_counted<AST::SyntheticNode>(argument.position(), entry));
            } else {
                auto values = TRY(list_of_values->resolve_as_list(this));
                for (auto& entry : values)
                    result.append(AST::make_ref_counted<AST::StringLiteral>(argument.position(), entry, AST::StringLiteral::EnclosureType::None));
            }
        }
    }

    return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), move(result));
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_filter_glob(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    // filter_glob string list
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly two arguments to filter_glob (<glob> <list>)", invoking_node.position());
        return nullptr;
    }

    auto glob_list = TRY(TRY(const_cast<AST::Node&>(arguments[0]).run(*this))->resolve_as_list(*this));
    if (glob_list.size() != 1) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the <glob> argument to filter_glob to be a single string", arguments[0].position());
        return nullptr;
    }
    auto& glob = glob_list.first();
    auto& list_node = arguments[1];

    NonnullRefPtrVector<AST::Node> result;

    TRY(const_cast<AST::Node&>(list_node).for_each_entry(*this, [&](NonnullRefPtr<AST::Value> entry) -> ErrorOr<IterationDecision> {
        auto value = TRY(entry->resolve_as_list(*this));
        if (value.size() == 0)
            return IterationDecision::Continue;
        if (value.size() == 1) {
            if (!value.first().bytes_as_string_view().matches(glob))
                return IterationDecision::Continue;
            result.append(AST::make_ref_counted<AST::StringLiteral>(arguments[1].position(), value.first(), AST::StringLiteral::EnclosureType::None));
            return IterationDecision::Continue;
        }

        for (auto& entry : value) {
            if (entry.bytes_as_string_view().matches(glob)) {
                NonnullRefPtrVector<AST::Node> nodes;
                for (auto& string : value)
                    nodes.append(AST::make_ref_counted<AST::StringLiteral>(arguments[1].position(), string, AST::StringLiteral::EnclosureType::None));
                result.append(AST::make_ref_counted<AST::ListConcatenate>(arguments[1].position(), move(nodes)));
                return IterationDecision::Continue;
            }
        }
        return IterationDecision::Continue;
    }));

    return AST::make_ref_counted<AST::ListConcatenate>(invoking_node.position(), move(result));
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_join(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to join", invoking_node.position());
        return nullptr;
    }

    auto delimiter = TRY(const_cast<AST::Node&>(arguments[0]).run(this));
    if (!delimiter->is_string()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the join delimiter string to be a string", arguments[0].position());
        return nullptr;
    }

    auto value = TRY(TRY(const_cast<AST::Node&>(arguments[1]).run(this))->resolve_without_cast(this));
    if (!value->is_list()) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected the joined list to be a list", arguments[1].position());
        return nullptr;
    }

    auto delimiter_str = TRY(delimiter->resolve_as_list(this))[0];
    StringBuilder builder;
    builder.join(delimiter_str, TRY(value->resolve_as_list(*this)));

    return AST::make_ref_counted<AST::StringLiteral>(invoking_node.position(), TRY(builder.to_string()), AST::StringLiteral::EnclosureType::None);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_value_or_default(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to value_or_default", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (!TRY(local_variable_or(name, ""sv)).is_empty())
        return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);

    return arguments.last();
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_assign_default(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to assign_default", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (!TRY(local_variable_or(name, ""sv)).is_empty())
        return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);

    auto value = TRY(TRY(const_cast<AST::Node&>(arguments.last()).run(*this))->resolve_without_cast(*this));
    set_local_variable(name.to_deprecated_string(), value);

    return make_ref_counted<AST::SyntheticNode>(invoking_node.position(), value);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_error_if_empty(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to error_if_empty", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (!TRY(local_variable_or(name, ""sv)).is_empty())
        return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);

    auto error_value = TRY(TRY(const_cast<AST::Node&>(arguments.last()).run(*this))->resolve_as_string(*this));
    if (error_value.is_empty())
        error_value = TRY(String::formatted("Expected {} to be non-empty", name));

    raise_error(ShellError::EvaluatedSyntaxError, error_value.bytes_as_string_view(), invoking_node.position());
    return nullptr;
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_null_or_alternative(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to null_or_alternative", invoking_node.position());
        return nullptr;
    }

    auto value = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_without_cast(*this));
    if ((value->is_string() && TRY(value->resolve_as_string(*this)).is_empty()) || (value->is_list() && TRY(value->resolve_as_list(*this)).is_empty()))
        return make_ref_counted<AST::SyntheticNode>(invoking_node.position(), value);

    return arguments.last();
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_defined_value_or_default(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to defined_value_or_default", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (!find_frame_containing_local_variable(name))
        return arguments.last();

    return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_assign_defined_default(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to assign_defined_default", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (find_frame_containing_local_variable(name))
        return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);

    auto value = TRY(TRY(const_cast<AST::Node&>(arguments.last()).run(*this))->resolve_without_cast(*this));
    set_local_variable(name.to_deprecated_string(), value);

    return make_ref_counted<AST::SyntheticNode>(invoking_node.position(), value);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_error_if_unset(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to error_if_unset", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (find_frame_containing_local_variable(name))
        return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);

    auto error_value = TRY(TRY(const_cast<AST::Node&>(arguments.last()).run(*this))->resolve_as_string(*this));
    if (error_value.is_empty())
        error_value = TRY(String::formatted("Expected {} to be set", name));

    raise_error(ShellError::EvaluatedSyntaxError, error_value.bytes_as_string_view(), invoking_node.position());
    return nullptr;
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_null_if_unset_or_alternative(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 2) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 2 arguments to null_if_unset_or_alternative", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    if (!find_frame_containing_local_variable(name))
        return arguments.last();

    return make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_reexpand(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 1) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 1 argument to reexpand", invoking_node.position());
        return nullptr;
    }

    auto value = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    return parse(value, m_is_interactive, false);
}

ErrorOr<RefPtr<AST::Node>> Shell::immediate_length_of_variable(AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
    if (arguments.size() != 1) {
        raise_error(ShellError::EvaluatedSyntaxError, "Expected exactly 1 argument to length_of_variable", invoking_node.position());
        return nullptr;
    }

    auto name = TRY(TRY(const_cast<AST::Node&>(arguments.first()).run(*this))->resolve_as_string(*this));
    auto variable = make_ref_counted<AST::SimpleVariable>(invoking_node.position(), name);

    return immediate_length_impl(
        invoking_node,
        { move(variable) },
        false);
}

ErrorOr<RefPtr<AST::Node>> Shell::run_immediate_function(StringView str, AST::ImmediateExpression& invoking_node, NonnullRefPtrVector<AST::Node> const& arguments)
{
#define __ENUMERATE_SHELL_IMMEDIATE_FUNCTION(name) \
    if (str == #name)                              \
        return immediate_##name(invoking_node, arguments);

    ENUMERATE_SHELL_IMMEDIATE_FUNCTIONS()

#undef __ENUMERATE_SHELL_IMMEDIATE_FUNCTION
    raise_error(ShellError::EvaluatedSyntaxError, DeprecatedString::formatted("Unknown immediate function {}", str), invoking_node.position());
    return nullptr;
}

bool Shell::has_immediate_function(StringView str)
{
#define __ENUMERATE_SHELL_IMMEDIATE_FUNCTION(name) \
    if (str == #name)                              \
        return true;

    ENUMERATE_SHELL_IMMEDIATE_FUNCTIONS()

#undef __ENUMERATE_SHELL_IMMEDIATE_FUNCTION

    return false;
}
}
