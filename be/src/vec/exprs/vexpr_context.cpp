// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/exprs/vexpr_context.h"

#include <algorithm>
#include <ostream>
#include <string>

// IWYU pragma: no_include <opentelemetry/common/threadlocal.h>
#include "common/compiler_util.h" // IWYU pragma: keep
#include "common/object_pool.h"
#include "runtime/runtime_state.h"
#include "runtime/thread_context.h"
#include "udf/udf.h"
#include "util/stack_util.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/columns_with_type_and_name.h"
#include "vec/exprs/vexpr.h"

namespace doris {
class RowDescriptor;
} // namespace doris

namespace doris::vectorized {
VExprContext::VExprContext(VExpr* expr)
        : _root(expr),
          _is_clone(false),
          _prepared(false),
          _opened(false),
          _closed(false),
          _last_result_column_id(-1) {}

VExprContext::~VExprContext() {
    // Do not delete this code, this code here is used to check if forget to close the opened context
    // Or there will be memory leak
    DCHECK(!_prepared || _closed) << get_stack_trace();
}

doris::Status VExprContext::execute(doris::vectorized::Block* block, int* result_column_id) {
    Status st;
    RETURN_IF_CATCH_EXCEPTION({
        st = _root->execute(this, block, result_column_id);
        _last_result_column_id = *result_column_id;
    });
    return st;
}

doris::Status VExprContext::prepare(doris::RuntimeState* state,
                                    const doris::RowDescriptor& row_desc) {
    _prepared = true;
    return _root->prepare(state, row_desc, this);
}

doris::Status VExprContext::open(doris::RuntimeState* state) {
    DCHECK(_prepared);
    if (_opened) {
        return Status::OK();
    }
    _opened = true;
    // Fragment-local state is only initialized for original contexts. Clones inherit the
    // original's fragment state and only need to have thread-local state initialized.
    FunctionContext::FunctionStateScope scope =
            _is_clone ? FunctionContext::THREAD_LOCAL : FunctionContext::FRAGMENT_LOCAL;
    return _root->open(state, this, scope);
}

void VExprContext::close(doris::RuntimeState* state) {
    DCHECK(!_closed);
    FunctionContext::FunctionStateScope scope =
            _is_clone ? FunctionContext::THREAD_LOCAL : FunctionContext::FRAGMENT_LOCAL;
    _root->close(state, this, scope);
    _closed = true;
}

doris::Status VExprContext::clone(RuntimeState* state, VExprContext** new_ctx) {
    DCHECK(_prepared) << "expr context not prepared";
    DCHECK(_opened);
    DCHECK(*new_ctx == nullptr);

    *new_ctx = state->obj_pool()->add(VExprContext::create_unique(_root).release());
    for (auto& _fn_context : _fn_contexts) {
        (*new_ctx)->_fn_contexts.push_back(_fn_context->clone());
    }

    (*new_ctx)->_is_clone = true;
    (*new_ctx)->_prepared = true;
    (*new_ctx)->_opened = true;

    return _root->open(state, *new_ctx, FunctionContext::THREAD_LOCAL);
}

void VExprContext::clone_fn_contexts(VExprContext* other) {
    for (auto& _fn_context : _fn_contexts) {
        other->_fn_contexts.push_back(_fn_context->clone());
    }
}

int VExprContext::register_function_context(RuntimeState* state,
                                            const doris::TypeDescriptor& return_type,
                                            const std::vector<doris::TypeDescriptor>& arg_types) {
    _fn_contexts.push_back(FunctionContext::create_context(state, return_type, arg_types));
    _fn_contexts.back()->set_check_overflow_for_decimal(state->check_overflow_for_decimal());
    return _fn_contexts.size() - 1;
}

Status VExprContext::filter_block(VExprContext* vexpr_ctx, Block* block, int column_to_keep) {
    if (vexpr_ctx == nullptr || block->rows() == 0) {
        return Status::OK();
    }
    int result_column_id = -1;
    RETURN_IF_ERROR(vexpr_ctx->execute(block, &result_column_id));
    return Block::filter_block(block, result_column_id, column_to_keep);
}

Status VExprContext::get_output_block_after_execute_exprs(
        const std::vector<vectorized::VExprContext*>& output_vexpr_ctxs, const Block& input_block,
        Block* output_block) {
    vectorized::Block tmp_block(input_block.get_columns_with_type_and_name());
    vectorized::ColumnsWithTypeAndName result_columns;
    for (auto vexpr_ctx : output_vexpr_ctxs) {
        int result_column_id = -1;
        RETURN_IF_ERROR(vexpr_ctx->execute(&tmp_block, &result_column_id));
        DCHECK(result_column_id != -1);
        result_columns.emplace_back(tmp_block.get_by_position(result_column_id));
    }
    *output_block = {result_columns};
    return Status::OK();
}

} // namespace doris::vectorized
