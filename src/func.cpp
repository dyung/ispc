/*
  Copyright (c) 2011-2023, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.


   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/** @file func.cpp
    @brief
*/

#include "func.h"
#include "ctx.h"
#include "expr.h"
#include "llvmutil.h"
#include "module.h"
#include "stmt.h"
#include "sym.h"
#include "type.h"
#include "util.h"

#include <stdio.h>

#include <llvm/IR/CFG.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/IPO.h>

#ifdef ISPC_XE_ENABLED
#include <llvm/GenXIntrinsics/GenXMetadata.h>
#endif

using namespace ispc;

bool Function::IsStdlibSymbol() const {
    if (sym == nullptr) {
        return false;
    }

    if (sym->pos.name != nullptr && !strcmp(sym->pos.name, "stdlib.ispc")) {
        return true;
    }
    return false;
}

void Function::debugPrintHelper(DebugPrintPoint dumpPoint) {
    if (code == nullptr || sym == nullptr) {
        return;
    }

    if (!g->debugPrint) {
        return;
    }

    // With debug prints enabled we will dump AST on several stages, so need annotation.
    if (g->debugPrint) {
        switch (dumpPoint) {
        case DebugPrintPoint::Initial:
            printf("Initial AST\n");
            break;
        case DebugPrintPoint::AfterTypeChecking:
            printf("AST after after typechecking\n");
            break;
        case DebugPrintPoint::AfterOptimization:
            printf("AST after optimization\n");
            break;
        }
    }

    Print();
    printf("\n");
}

void Function::Print() const {
    Indent indent;
    indent.pushSingle();
    Print(indent);
    fflush(stdout);
}

void Function::Print(Indent &indent) const {
    indent.Print("Function");

    if (sym && sym->type) {
        sym->pos.Print();
        printf(" [%s] \"%s\"\n", sym->type->GetString().c_str(), sym->name.c_str());
    } else {
        printf("<NULL>\n");
    }

    indent.pushList(args.size() + 1);
    for (int i = 0; i < args.size(); i++) {
        static constexpr std::size_t BUFSIZE{15};
        char buffer[BUFSIZE];
        snprintf(buffer, BUFSIZE, "param %d", i);
        indent.setNextLabel(buffer);
        if (args[i]) {
            indent.Print();
            if (args[i]->type != nullptr) {
                printf("[%s] ", args[i]->type->GetString().c_str());
            }
            printf("%s\n", args[i]->name.c_str());
            indent.Done();
        } else {
            indent.Print("<NULL>\n");
            indent.Done();
        }
    }

    indent.setNextLabel("body");
    if (code != nullptr) {
        code->Print(indent);
    } else {
        printf("<CODE is missing>\n");
    }
    indent.Done();
}

// The Function is created when the body of the function is already parsed and AST is created for it,
// and we are about to close the symbol table scope for the function. So all symbols that require special
// handling during code generation must be saved. This includes symbols for arguments and special symbols
// like __mask and thread / task variables.
// Type checking and optimization is also done here.
Function::Function(Symbol *s, Stmt *c) : sym(s), code(c) {
    maskSymbol = m->symbolTable->LookupVariable("__mask");
    Assert(maskSymbol != NULL);

    const FunctionType *type = CastType<FunctionType>(sym->type);
    Assert(type != NULL);

    for (int i = 0; i < type->GetNumParameters(); ++i) {
        const char *paramName = type->GetParameterName(i).c_str();
        Symbol *paramSym = m->symbolTable->LookupVariable(paramName);
        if (paramSym == NULL)
            Assert(strncmp(paramName, "__anon_parameter_", 17) == 0);
        args.push_back(paramSym);

        const Type *t = type->GetParameterType(i);
        if (paramSym != NULL && CastType<ReferenceType>(t) == NULL)
            paramSym->parentFunction = this;
    }

    if (type->isTask) {
        threadIndexSym = m->symbolTable->LookupVariable("threadIndex");
        Assert(threadIndexSym);
        threadCountSym = m->symbolTable->LookupVariable("threadCount");
        Assert(threadCountSym);
        taskIndexSym = m->symbolTable->LookupVariable("taskIndex");
        Assert(taskIndexSym);
        taskCountSym = m->symbolTable->LookupVariable("taskCount");
        Assert(taskCountSym);

        taskIndexSym0 = m->symbolTable->LookupVariable("taskIndex0");
        Assert(taskIndexSym0);
        taskIndexSym1 = m->symbolTable->LookupVariable("taskIndex1");
        Assert(taskIndexSym1);
        taskIndexSym2 = m->symbolTable->LookupVariable("taskIndex2");
        Assert(taskIndexSym2);

        taskCountSym0 = m->symbolTable->LookupVariable("taskCount0");
        Assert(taskCountSym0);
        taskCountSym1 = m->symbolTable->LookupVariable("taskCount1");
        Assert(taskCountSym1);
        taskCountSym2 = m->symbolTable->LookupVariable("taskCount2");
        Assert(taskCountSym2);
    } else {
        threadIndexSym = threadCountSym = taskIndexSym = taskCountSym = NULL;
        taskIndexSym0 = taskIndexSym1 = taskIndexSym2 = NULL;
        taskCountSym0 = taskCountSym1 = taskCountSym2 = NULL;
    }

    typeCheckAndOptimize();
}

void Function::typeCheckAndOptimize() {
    if (code != NULL) {
        debugPrintHelper(DebugPrintPoint::Initial);

        code = TypeCheck(code);

        debugPrintHelper(DebugPrintPoint::AfterTypeChecking);

        if (code != NULL) {
            code = Optimize(code);

            debugPrintHelper(DebugPrintPoint::AfterOptimization);
        }
    }
}

const Type *Function::GetReturnType() const {
    const FunctionType *type = CastType<FunctionType>(sym->type);
    Assert(type != NULL);
    return type->GetReturnType();
}

const FunctionType *Function::GetType() const {
    const FunctionType *type = CastType<FunctionType>(sym->type);
    Assert(type != NULL);
    return type;
}

/** Parameters for tasks are stored in a big structure; this utility
    function emits code to copy those values out of the task structure into
    local stack-allocated variables.  (Which we expect that LLVM's
    'mem2reg' pass will in turn promote to SSA registers..
 */
static void lCopyInTaskParameter(int i, AddressInfo *structArgPtrInfo, const std::vector<Symbol *> &args,
                                 FunctionEmitContext *ctx) {
    // We expect the argument structure to come in as a poitner to a
    // structure.  Confirm and figure out its type here.
    const llvm::Type *structArgType = structArgPtrInfo->getPointer()->getType();
    Assert(llvm::isa<llvm::PointerType>(structArgType));
    const llvm::PointerType *pt = llvm::dyn_cast<const llvm::PointerType>(structArgType);
    Assert(pt);
    Assert(llvm::isa<llvm::StructType>(structArgPtrInfo->getElementType()));

    // Get the type of the argument we're copying in and its Symbol pointer
    Symbol *sym = args[i];

    if (sym == NULL)
        // anonymous parameter, so don't worry about it
        return;

    // allocate space to copy the parameter in to
    sym->storageInfo = ctx->AllocaInst(sym->type, sym->name.c_str());
    Assert(sym->storageInfo);

    // get a pointer to the value in the struct
    llvm::Value *ptr = ctx->AddElementOffset(structArgPtrInfo, i, sym->name.c_str());

    // and copy the value from the struct and into the local alloca'ed
    // memory
    llvm::Value *ptrval =
        ctx->LoadInst(new AddressInfo(ptr, sym->storageInfo->getElementType()), sym->type, sym->name.c_str());
    ctx->StoreInst(ptrval, sym->storageInfo, sym->type);
    ctx->EmitFunctionParameterDebugInfo(sym, i);
}

static llvm::Value *lXeGetTaskVariableValue(FunctionEmitContext *ctx, std::string taskFunc) {
    std::vector<llvm::Value *> args;
    llvm::Function *task_func = m->module->getFunction(taskFunc);
    Assert(task_func != NULL);
    return ctx->CallInst(task_func, NULL, args, taskFunc + "_call");
}

/** Given the statements implementing a function, emit the code that
    implements the function.  Most of the work do be done here just
    involves wiring up the function parameter values to be available in the
    function body code.
 */
void Function::emitCode(FunctionEmitContext *ctx, llvm::Function *function, SourcePos firstStmtPos) {
    // Connect the __mask builtin to the location in memory that stores its
    // value
    maskSymbol->storageInfo = ctx->GetFullMaskAddressInfo();

    // add debugging info for __mask
    maskSymbol->pos = firstStmtPos;
    ctx->EmitVariableDebugInfo(maskSymbol);

    if (g->NoOmitFramePointer)
        function->addFnAttr("no-frame-pointer-elim", "true");
    if (g->target->getArch() == Arch::wasm32)
        function->addFnAttr("target-features", "+simd128");

    g->target->markFuncWithTargetAttr(function);
#if 0
    llvm::BasicBlock *entryBBlock = ctx->GetCurrentBasicBlock();
#endif
    const FunctionType *type = CastType<FunctionType>(sym->type);
    Assert(type != NULL);

    // CPU tasks
    if (type->isTask == true && !g->target->isXeTarget()) {
        Assert(type->IsISPCExternal() == false);
        // For tasks, there should always be three parameters: the
        // pointer to the structure that holds all of the arguments, the
        // thread index, and the thread count variables.
        llvm::Function::arg_iterator argIter = function->arg_begin();

        llvm::Value *structParamPtr = &*(argIter++);
        llvm::Value *threadIndex = &*(argIter++);
        llvm::Value *threadCount = &*(argIter++);
        llvm::Value *taskIndex = &*(argIter++);
        llvm::Value *taskCount = &*(argIter++);
        llvm::Value *taskIndex0 = &*(argIter++);
        llvm::Value *taskIndex1 = &*(argIter++);
        llvm::Value *taskIndex2 = &*(argIter++);
        llvm::Value *taskCount0 = &*(argIter++);
        llvm::Value *taskCount1 = &*(argIter++);
        llvm::Value *taskCount2 = &*(argIter++);

        std::vector<llvm::Type *> llvmArgTypes = type->LLVMFunctionArgTypes(g->ctx);
        llvm::Type *st = llvm::StructType::get(*g->ctx, llvmArgTypes);
        AddressInfo *stInfo = new AddressInfo(structParamPtr, st);
        // Copy the function parameter values from the structure into local
        // storage
        for (unsigned int i = 0; i < args.size(); ++i)
            lCopyInTaskParameter(i, stInfo, args, ctx);

        if (type->isUnmasked == false) {
            // Copy in the mask as well.
            int nArgs = (int)args.size();
            // The mask is the last parameter in the argument structure
            llvm::Value *ptr = ctx->AddElementOffset(stInfo, nArgs, "task_struct_mask");
            llvm::Value *ptrval = ctx->LoadInst(new AddressInfo(ptr, LLVMTypes::MaskType), NULL, "mask");
            ctx->SetFunctionMask(ptrval);
        }

        // Copy threadIndex and threadCount into stack-allocated storage so
        // that their symbols point to something reasonable.
        threadIndexSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "threadIndex");
        ctx->StoreInst(threadIndex, threadIndexSym->storageInfo);

        threadCountSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "threadCount");
        ctx->StoreInst(threadCount, threadCountSym->storageInfo);

        // Copy taskIndex and taskCount into stack-allocated storage so
        // that their symbols point to something reasonable.
        taskIndexSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex");
        ctx->StoreInst(taskIndex, taskIndexSym->storageInfo);

        taskCountSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount");
        ctx->StoreInst(taskCount, taskCountSym->storageInfo);

        taskIndexSym0->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex0");
        ctx->StoreInst(taskIndex0, taskIndexSym0->storageInfo);
        taskIndexSym1->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex1");
        ctx->StoreInst(taskIndex1, taskIndexSym1->storageInfo);
        taskIndexSym2->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex2");
        ctx->StoreInst(taskIndex2, taskIndexSym2->storageInfo);

        taskCountSym0->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount0");
        ctx->StoreInst(taskCount0, taskCountSym0->storageInfo);
        taskCountSym1->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount1");
        ctx->StoreInst(taskCount1, taskCountSym1->storageInfo);
        taskCountSym2->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount2");
        ctx->StoreInst(taskCount2, taskCountSym2->storageInfo);
    } else {
        // Regular, non-task function or GPU task
        llvm::Function::arg_iterator argIter = function->arg_begin();
        llvm::FunctionType *fType = type->LLVMFunctionType(g->ctx);
        Assert(fType->getFunctionNumParams() >= args.size());
        for (unsigned int i = 0; i < args.size(); ++i, ++argIter) {
            Symbol *argSym = args[i];
            if (argSym == NULL)
                // anonymous function parameter
                continue;

            argIter->setName(argSym->name.c_str());

            // Allocate stack storage for the parameter and emit code
            // to store the its value there.
            argSym->storageInfo = ctx->AllocaInst(argSym->type, argSym->name.c_str());
            // ISPC export and extern "C" functions have addrspace in the declaration on Xe so
            // we cast addrspace from generic to default in the alloca BB.
            // define dso_local spir_func void @test(%S addrspace(4)* noalias %s)
            // addrspacecast %S addrspace(4)* %s to %S*
            llvm::Value *addrCasted = &*argIter;
#ifdef ISPC_XE_ENABLED
            // Update addrspace of passed argument if needed for Xe target
            if (g->target->isXeTarget()) {
                addrCasted = ctx->XeUpdateAddrSpaceForParam(addrCasted, fType, i, true);
            }
#endif

            ctx->StoreInst(addrCasted, argSym->storageInfo, argSym->type);

            ctx->EmitFunctionParameterDebugInfo(argSym, i);
        }

        // If the number of actual function arguments is equal to the
        // number of declared arguments in decl->functionParams, then we
        // don't have a mask parameter, so set it to be all on.  This
        // happens for example with 'export'ed functions that the app
        // calls, with tasks on GPU and with unmasked functions.
        if (argIter == function->arg_end()) {
            Assert(type->isUnmasked || type->isExported || type->isExternC || type->isExternSYCL ||
                   type->IsISPCExternal() || type->IsISPCKernel());
            ctx->SetFunctionMask(LLVMMaskAllOn);
        } else {
            Assert(type->isUnmasked == false);

            // Otherwise use the mask to set the entry mask value
            argIter->setName("__mask");
            Assert(argIter->getType() == LLVMTypes::MaskType);

            if (ctx->emitXeHardwareMask()) {
                // We should not create explicit predication
                // to avoid EM usage duplication. All stuff
                // will be done by SIMD CF Lowering
                // TODO: temporary workaround that will be changed
                // as part of SPIR-V emitting solution
                ctx->SetFunctionMask(LLVMMaskAllOn);
            } else {
                ctx->SetFunctionMask(&*argIter);
            }

            ++argIter;
            Assert(argIter == function->arg_end());
        }
        if (g->target->isXeTarget() && type->isTask) {
            // Assign threadIndex and threadCount to the result of calling of corresponding builtins.
            // On Xe threadIndex equals to taskIndex and threadCount to taskCount.
            threadIndexSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "threadIndex");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_index"), threadIndexSym->storageInfo);

            threadCountSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "threadCount");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_count"), threadCountSym->storageInfo);

            // Assign taskIndex and taskCount to the result of calling of corresponding builtins.
            taskIndexSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_index"), taskIndexSym->storageInfo);

            taskCountSym->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_count"), taskCountSym->storageInfo);

            taskIndexSym0->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex0");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_index0"), taskIndexSym0->storageInfo);
            taskIndexSym1->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex1");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_index1"), taskIndexSym1->storageInfo);
            taskIndexSym2->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskIndex2");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_index2"), taskIndexSym2->storageInfo);

            taskCountSym0->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount0");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_count0"), taskCountSym0->storageInfo);
            taskCountSym1->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount1");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_count1"), taskCountSym1->storageInfo);
            taskCountSym2->storageInfo = ctx->AllocaInst(LLVMTypes::Int32Type, "taskCount2");
            ctx->StoreInst(lXeGetTaskVariableValue(ctx, "__task_count2"), taskCountSym2->storageInfo);
        }
    }

    // Set FTZ/DAZ flags if requested
    ctx->SetFunctionFTZ_DAZFlags();

    // Finally, we can generate code for the function
    if (code != NULL) {
        ctx->SetDebugPos(code->pos);
        ctx->AddInstrumentationPoint("function entry");

        int costEstimate = EstimateCost(code);
        Debug(code->pos, "Estimated cost for function \"%s\" = %d\n", sym->name.c_str(), costEstimate);

        // If the body of the function is non-trivial, then we wrap the
        // entire thing inside code that tests to see if the mask is all
        // on, all off, or mixed.  If this is a simple function, then this
        // isn't worth the code bloat / overhead.
        bool checkMask =
            (!g->target->isXeTarget() && type->isTask == true) ||
#if ISPC_LLVM_VERSION >= ISPC_LLVM_14_0
            ((function->getAttributes().getFnAttrs().hasAttribute(llvm::Attribute::AlwaysInline) == false) &&
#else
            ((function->getAttributes().getFnAttributes().hasAttribute(llvm::Attribute::AlwaysInline) == false) &&
#endif
             costEstimate > CHECK_MASK_AT_FUNCTION_START_COST);
        checkMask &= (type->isUnmasked == false);
        checkMask &= (g->target->getMaskingIsFree() == false);
        checkMask &= (g->opt.disableCoherentControlFlow == false);

        if (checkMask) {
            llvm::Value *mask = ctx->GetFunctionMask();
            llvm::Value *allOn = ctx->All(mask);
            llvm::BasicBlock *bbAllOn = ctx->CreateBasicBlock("all_on");
            llvm::BasicBlock *bbSomeOn = ctx->CreateBasicBlock("some_on");

            // Set up basic blocks for goto targets
            ctx->InitializeLabelMap(code);

            ctx->BranchInst(bbAllOn, bbSomeOn, allOn);
            // all on: we've determined dynamically that the mask is all
            // on.  Set the current mask to "all on" explicitly so that
            // codegen for this path can be improved with this knowledge in
            // hand...
            ctx->SetCurrentBasicBlock(bbAllOn);
            if (!g->opt.disableMaskAllOnOptimizations)
                ctx->SetFunctionMask(LLVMMaskAllOn);
            code->EmitCode(ctx);
            if (ctx->GetCurrentBasicBlock())
                ctx->ReturnInst();

            // not all on: however, at least one lane must be running,
            // since we should never run with all off...  some on: reset
            // the mask to the value it had at function entry and emit the
            // code.  Resetting the mask here is important, due to the "all
            // on" setting of it for the path above.
            ctx->SetCurrentBasicBlock(bbSomeOn);
            ctx->SetFunctionMask(mask);

            // Set up basic blocks for goto targets again; we want to have
            // one set of them for gotos in the 'all on' case, and a
            // distinct set for the 'mixed mask' case.
            ctx->InitializeLabelMap(code);

            code->EmitCode(ctx);
            if (ctx->GetCurrentBasicBlock())
                ctx->ReturnInst();
        } else {
            // Set up basic blocks for goto targets
            ctx->InitializeLabelMap(code);
            // No check, just emit the code
            code->EmitCode(ctx);
        }
    }

    if (ctx->GetCurrentBasicBlock()) {
        // FIXME: We'd like to issue a warning if we've reached the end of
        // the function without a return statement (for non-void
        // functions).  But the test below isn't right, since we can have
        // (with 'x' a varying test) "if (x) return a; else return b;", in
        // which case we have a valid basic block but its unreachable so ok
        // to not have return statement.
#if 0
        // If the bblock has no predecessors, then it doesn't matter if it
        // doesn't have a return; it'll never be reached.  If it does,
        // issue a warning.  Also need to warn if it's the entry block for
        // the function (in which case it will not have predeccesors but is
        // still reachable.)
        if (type->GetReturnType()->IsVoidType() == false &&
            (pred_begin(ec.bblock) != pred_end(ec.bblock) || (ec.bblock == entryBBlock)))
            Warning(sym->pos, "Missing return statement in function returning \"%s\".",
                    type->rType->GetString().c_str());
#endif

        // FIXME: would like to set the context's current position to
        // e.g. the end of the function code

        // if bblock is non-NULL, it hasn't been terminated by e.g. a
        // return instruction.  Need to add a return instruction.
        ctx->ReturnInst();
    }
#ifdef ISPC_XE_ENABLED
    if (type->IsISPCKernel()) {
        // Emit metadata for XE kernel

        llvm::LLVMContext &fContext = function->getContext();
        llvm::NamedMDNode *mdKernels = m->module->getOrInsertNamedMetadata("genx.kernels");

        std::string AsmName =
            (m->module->getName() + llvm::Twine('_') + llvm::Twine(mdKernels->getNumOperands()) + llvm::Twine(".asm"))
                .str();

        // Kernel arg kinds
        llvm::Type *i32Type = llvm::Type::getInt32Ty(fContext);
        llvm::SmallVector<llvm::Metadata *, 8> argKinds;
        llvm::SmallVector<llvm::Metadata *, 8> argInOutKinds;
        llvm::SmallVector<llvm::Metadata *, 8> argTypeDescs;

        // In ISPC we need only AK_NORMAL and IK_NORMAL now, in future it can change.
        enum { AK_NORMAL, AK_SAMPLER, AK_SURFACE, AK_VME };
        enum { IK_NORMAL, IK_INPUT, IK_OUTPUT, IK_INPUT_OUTPUT };
        unsigned int offset = 32;
        unsigned int grf_size = g->target->getXeGrfSize();
        for (int i = 0; i < args.size(); i++) {
            const Type *T = args[i]->type;
            argKinds.push_back(llvm::ValueAsMetadata::get(llvm::ConstantInt::get(i32Type, AK_NORMAL)));
            argInOutKinds.push_back(llvm::ValueAsMetadata::get(llvm::ConstantInt::get(i32Type, IK_NORMAL)));
            llvm::Type *argType = function->getArg(i)->getType();
            if (argType->isPtrOrPtrVectorTy() || argType->isArrayTy()) {
                argTypeDescs.push_back(llvm::MDString::get(fContext, llvm::StringRef("svmptr_t read_write")));
            } else {
                argTypeDescs.push_back(llvm::MDString::get(fContext, llvm::StringRef("")));
            }

            llvm::Type *type = T->LLVMType(&fContext);
            unsigned bytes = type->getScalarSizeInBits() / 8;
            if (bytes != 0) {
                offset = llvm::alignTo(offset, bytes);
            }

            if (llvm::isa<llvm::VectorType>(type)) {
                bytes = type->getPrimitiveSizeInBits() / 8;

                if ((offset & (grf_size - 1)) + bytes > grf_size)
                    // GRF align if arg would cross GRF boundary
                    offset = llvm::alignTo(offset, grf_size);
            }

            offset += bytes;
        }

        // TODO: Number of fields is 9 now, and it is a magic number that seems
        // to be not defined anywhere. Consider changing it when possible.
        llvm::SmallVector<llvm::Metadata *, 9> mdArgs(9, nullptr);
        mdArgs[llvm::genx::KernelMDOp::FunctionRef] = llvm::ValueAsMetadata::get(function);
        mdArgs[llvm::genx::KernelMDOp::Name] = llvm::MDString::get(fContext, sym->name);
        mdArgs[llvm::genx::KernelMDOp::ArgKinds] = llvm::MDNode::get(fContext, argKinds);
        mdArgs[llvm::genx::KernelMDOp::SLMSize] = llvm::ValueAsMetadata::get(llvm::ConstantInt::getNullValue(i32Type));
        mdArgs[llvm::genx::KernelMDOp::ArgOffsets] =
            llvm::ValueAsMetadata::get(llvm::ConstantInt::getNullValue(i32Type));
        mdArgs[llvm::genx::KernelMDOp::ArgIOKinds] = llvm::MDNode::get(fContext, argInOutKinds);
        mdArgs[llvm::genx::KernelMDOp::ArgTypeDescs] = llvm::MDNode::get(fContext, argTypeDescs);
        mdArgs[llvm::genx::KernelMDOp::NBarrierCnt] =
            llvm::ValueAsMetadata::get(llvm::ConstantInt::getNullValue(i32Type));
        mdArgs[llvm::genx::KernelMDOp::BarrierCnt] =
            llvm::ValueAsMetadata::get(llvm::ConstantInt::getNullValue(i32Type));

        mdKernels->addOperand(llvm::MDNode::get(fContext, mdArgs));
        // This is needed to run in L0 runtime.
        function->addFnAttr("oclrt", "1");
    }
#endif
}

void Function::GenerateIR() {
    if (sym == NULL)
        // May be NULL due to error earlier in compilation
        return;

    llvm::Function *function = sym->function;
    Assert(function != NULL);

    // But if that function has a definition, we don't want to redefine it.
    if (function->empty() == false) {
        Error(sym->pos, "Ignoring redefinition of function \"%s\".", sym->name.c_str());
        return;
    }

    const FunctionType *type = CastType<FunctionType>(sym->type);
    Assert(type != NULL);

    if (type->isExternSYCL) {
        Error(sym->pos, "\n\'extern \"SYCL\"\' function \"%s\" cannot be defined in ISPC.", sym->name.c_str());
        return;
    }

    // Figure out a reasonable source file position for the start of the
    // function body.  If possible, get the position of the first actual
    // non-StmtList statment...
    SourcePos firstStmtPos = sym->pos;
    if (code) {
        StmtList *sl = llvm::dyn_cast<StmtList>(code);
        if (sl && sl->stmts.size() > 0 && sl->stmts[0] != NULL)
            firstStmtPos = sl->stmts[0]->pos;
        else
            firstStmtPos = code->pos;
    }
    // And we can now go ahead and emit the code
    if (g->target->isXeTarget()) {
        // For Xe target we do not emit code for masked version of a function
        // if it is a kernel
        const FunctionType *type = CastType<FunctionType>(sym->type);
        if (!type->IsISPCKernel()) {
            llvm::TimeTraceScope TimeScope("emitCode", llvm::StringRef(sym->name));
            FunctionEmitContext ec(this, sym, function, firstStmtPos);
            emitCode(&ec, function, firstStmtPos);
        }
    } else {
        // In case of multi-target compilation for extern "C" functions which were defined, we want
        // to have a target-specific implementation for each target similar to exported functions.
        // However declarations of extern "C"/"SYCL" functions must be not-mangled and therefore, the calls to such
        // functions must be not-mangled. The trick to support target-specific implementation in such case is to
        // generate definition of target-specific implementation mangled with target ("name_<target>") which would be
        // called from a dispatch function. Since we use not-mangled names in the call, it will be a call to a dispatch
        // function which will resolve to particular implementation. The condition below ensures that in case of
        // multi-target compilation we will emit only one-per-target definition of extern "C" function mangled with
        // <target> suffix.
        if (!((type->isExternC || type->isExternSYCL) && g->mangleFunctionsWithTarget)) {
            llvm::TimeTraceScope TimeScope("emitCode", llvm::StringRef(sym->name));
            FunctionEmitContext ec(this, sym, function, firstStmtPos);
            emitCode(&ec, function, firstStmtPos);
        }
    }

    if (m->errorCount == 0) {
        // If the function is 'export'-qualified, emit a second version of
        // it without a mask parameter and without name mangling so that
        // the application can call it.
        // For 'extern "C"' we emit the version without mask parameter only.
        // For Xe we emit a version without mask parameter only for ISPC kernels and
        // ISPC external functions.
        if (type->isExported || type->isExternC || type->isExternSYCL || type->IsISPCExternal() ||
            type->IsISPCKernel()) {
            llvm::FunctionType *ftype = type->LLVMFunctionType(g->ctx, true);
            llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::ExternalLinkage;
            auto [name_pref, name_suf] = type->GetFunctionMangledName(true);
            std::string functionName = name_pref + sym->name + name_suf;

            llvm::Function *appFunction = llvm::Function::Create(ftype, linkage, functionName.c_str(), m->module);
            appFunction->setDoesNotThrow();
            appFunction->setCallingConv(type->GetCallingConv());

            // Xe kernel should have "dllexport" and "CMGenxMain" attribute,
            // otherss have "CMStackCall" attribute
            if (g->target->isXeTarget()) {
                if (type->IsISPCExternal()) {
                    appFunction->addFnAttr("CMStackCall");

                } else if (type->IsISPCKernel()) {
                    appFunction->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
                    appFunction->addFnAttr("CMGenxMain");
                }
            } else {
                // Make application function callable from DLLs.
                if ((g->target_os == TargetOS::windows) && (g->dllExport)) {
                    appFunction->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
                }
            }

            if (function->getFunctionType()->getNumParams() > 0) {
                for (int i = 0; i < function->getFunctionType()->getNumParams() - 1; i++) {
                    if (function->hasParamAttribute(i, llvm::Attribute::NoAlias)) {
                        appFunction->addParamAttr(i, llvm::Attribute::NoAlias);
                    }
                }
            }
            g->target->markFuncWithTargetAttr(appFunction);

            if (appFunction->getName() != functionName) {
                // this was a redefinition for which we already emitted an
                // error, so don't worry about this one...
                appFunction->eraseFromParent();
            } else {
                llvm::TimeTraceScope TimeScope("emitCode", llvm::StringRef(sym->name));
                // And emit the code again
                FunctionEmitContext ec(this, sym, appFunction, firstStmtPos);
                emitCode(&ec, appFunction, firstStmtPos);
                if (m->errorCount == 0) {
                    sym->exportedFunction = appFunction;
                }
            }
        } else {
            if (g->target->isXeTarget()) {
                // Mark all internal ISPC functions as a stack call
                function->addFnAttr("CMStackCall");
                // Mark all internal ISPC functions as AlwaysInline to facilitate inlining on GPU
                // if it's not marked as "noinline" explicitly
#if ISPC_LLVM_VERSION >= ISPC_LLVM_14_0
                if (!(function->getAttributes().getFnAttrs().hasAttribute(llvm::Attribute::NoInline) ||
                      function->getAttributes().getFnAttrs().hasAttribute(llvm::Attribute::AlwaysInline)))
#else
                if (!(function->getAttributes().getFnAttributes().hasAttribute(llvm::Attribute::NoInline) ||
                      function->getAttributes().getFnAttributes().hasAttribute(llvm::Attribute::AlwaysInline)))
#endif
                {
                    function->addFnAttr(llvm::Attribute::AlwaysInline);
                }
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////
// TemplateParms

TemplateParms::TemplateParms() {}

void TemplateParms::Add(const TemplateTypeParmType *p) { parms.push_back(p); }

size_t TemplateParms::GetCount() const { return parms.size(); }

const TemplateTypeParmType *TemplateParms::operator[](size_t i) const { return parms[i]; }

bool TemplateParms::IsEqual(const TemplateParms *p) const {
    if (p == nullptr) {
        return false;
    }

    if (GetCount() != p->GetCount()) {
        return false;
    }

    for (size_t i = 0; i < GetCount(); i++) {
        if (!Type::Equal((*this)[i], (*p)[i])) {
            return false;
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////
// TemplateArgs

TemplateArgs::TemplateArgs(const std::vector<std::pair<const Type *, SourcePos>> &a) : args(a) {}

bool TemplateArgs::IsEqual(TemplateArgs &otherArgs) const {
    if (args.size() != otherArgs.args.size()) {
        return false;
    }
    for (int i = 0; i < args.size(); i++) {
        if (!Type::Equal(args[i].first, otherArgs.args[i].first)) {
            return false;
        }
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////
// TemplateInstantiation

TemplateInstantiation::TemplateInstantiation(const TemplateParms &typeParms,
                                             const std::vector<std::pair<const Type *, SourcePos>> &typeArgs)
    : functionSym(nullptr) {
    Assert(typeArgs.size() == typeParms.GetCount());
    for (int i = 0; i < typeArgs.size(); i++) {
        std::string name = typeParms[i]->GetName();
        const Type *type = typeArgs[i].first;
        args[name] = type;
    }
}

const Type *TemplateInstantiation::InstantiateType(const std::string &name) {
    auto t = args.find(name);
    if (t == args.end()) {
        return nullptr;
    }

    return t->second;
}

Symbol *TemplateInstantiation::InstantiateSymbol(Symbol *sym) {
    if (sym == nullptr) {
        return nullptr;
    }

    // A note about about global symbols.
    // In the current state of symbol table there's no clear way to differentiate between global and local symbols.
    // There's "parentFunction" field, but it's empty for some local symbols and paramters, which prevents using it
    // for the purpose of differentiation.
    // There's another possible way to differentiate - "storageInfo" tends to be set only for global symbols, but again
    // it's inderent and unreliable way to detect what needs to be encoded explicitly.
    // So we copy all symbols - global and local, while we need not avoid copying globals.
    // TODO: develop a reliable mechanism to detect global symbols and do not copy them.

    auto t = symMap.find(sym);
    if (t != symMap.end()) {
        return t->second;
    }

    const Type *instType = sym->type->ResolveDependence(*this);
    Symbol *instSym = new Symbol(sym->name, sym->pos, instType, sym->storageClass);
    instSym->constValue = sym->constValue ? sym->constValue->Instantiate(*this) : nullptr;
    instSym->varyingCFDepth = sym->varyingCFDepth;
    instSym->parentFunction = nullptr;
    instSym->storageInfo = sym->storageInfo;

    symMap.emplace(std::make_pair(sym, instSym));
    return instSym;
}

// After the instance of the template function is created, the symbols should point to the parent function.
void TemplateInstantiation::SetFunction(Function *func) {
    for (auto &symPair : symMap) {
        Symbol *sym = symPair.second;
        sym->parentFunction = func;
    }
    functionSym->parentFunction = func;
}
