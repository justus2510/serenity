/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerNavigator.h>

namespace Web::HTML {

WebIDL::ExceptionOr<JS::NonnullGCPtr<WorkerNavigator>> WorkerNavigator::create(WorkerGlobalScope& global_scope)
{
    return MUST_OR_THROW_OOM(global_scope.heap().allocate<WorkerNavigator>(global_scope.realm(), global_scope));
}

WorkerNavigator::WorkerNavigator(WorkerGlobalScope& global_scope)
    : PlatformObject(global_scope.realm())
{
}

WorkerNavigator::~WorkerNavigator() = default;

JS::ThrowCompletionOr<void> WorkerNavigator::initialize(JS::Realm& realm)
{
    MUST_OR_THROW_OOM(Base::initialize(realm));
    set_prototype(&Bindings::ensure_web_prototype<Bindings::WorkerNavigatorPrototype>(realm, "WorkerNavigator"));

    return {};
}

}
