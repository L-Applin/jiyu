
// :AboutGetFinalType:
// Code generation does not care about type system shenanigans,
// so almost all cases of get_final_type have been replaced with
// get_underlying_final_type. There are a few instances where
// respecting the type system is necessary, and that primarily includes
// debug info generation, since some debug info formats respect
// that a C typedef is a proper renaming of the type for the user's
// sake, and so will respect that a typealias is a meaningful
// renaming.

#include "llvm.h"
#include "ast.h"
#include "compiler.h"

// We dont need or care about a wall of warnings from LLVM code.
#ifdef WIN32
#pragma warning(push, 0)
#endif

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"

#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/JITSymbol.h"

#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/Transforms/Utils.h"

#ifdef WIN32
#pragma warning(pop)
#endif

using namespace llvm;
using namespace llvm::orc;

static StringRef string_ref(String s) {
    return StringRef(s.data, s.length);
}

DIFile *get_debug_file(LLVMContext *ctx, Ast *ast) {
    return DIFile::get(*ctx, string_ref(ast->filename), "");
}

string_length_type get_line_number(Ast *ast) {
    // @Speed we should probably just track line_start on Ast itself
    // and have the lexer set this on tokens.
    string_length_type line_start = 0, char_start = 0, line_end = 0, char_end = 0;
    ast->text_span.calculate_text_coordinates(&line_start, &char_start, &line_end, &char_end);
    return line_start;
}

void LLVM_Generator::preinit() {
    // This gets called before Compiler is fully initialized so that Compiler
    // can query TargetMachine to configure string and arrays to be the right
    // size on 32-bit vs 64-bit machines.
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    InitializeAllAsmPrinters();

    std::string default_target_triple = llvm::sys::getDefaultTargetTriple();
    std::string process_triple = llvm::sys::getProcessTriple();

    std::string TargetTriple = default_target_triple;
    if (compiler->is_metaprogram) {
        TargetTriple = process_triple;
    }
    if (compiler->build_options.target_triple.length) {
        TargetTriple = to_c_string(compiler->build_options.target_triple); // @Leak
    }
    TargetTriple = Triple::normalize(TargetTriple);

    if (compiler->build_options.verbose_diagnostics) {
        // @TODO we should have a compiler->print_diagnostic function
        printf("w%" PRId64 ": LLVM default target: %s\n",        compiler->instance_number, default_target_triple.c_str());
        printf("w%" PRId64 ": LLVM process target: %s\n",        compiler->instance_number, process_triple.c_str());
        printf("w%" PRId64 ": LLVM target: %s\n",                compiler->instance_number, TargetTriple.c_str());
    }

    std::string Error;
    auto Target = TargetRegistry::lookupTarget(TargetTriple, Error);

    // Print an error and exit if we couldn't find the requested target.
    // This generally occurs if we've forgotten to initialise the
    // TargetRegistry or we have a bogus target triple.
    if (!Target) {
        compiler->report_error((Ast *)nullptr, "LLVM error: %s\n", Error.c_str());
        return;
    }

    auto CPU = "generic";
    auto Features = "";

    TargetOptions opt;
    auto RM = Optional<Reloc::Model>();
    // RM = Reloc::Model::PIC_;
    TargetMachine = Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);
}

void LLVM_Generator::init() {
    auto ctx = llvm::make_unique<LLVMContext>();
    thread_safe_context = new ThreadSafeContext(std::move(ctx));
    llvm_context = thread_safe_context->getContext();

    llvm_module = new Module("jiyu Module", *llvm_context);

    irb = new IRBuilder<>(*llvm_context);
    dib = new DIBuilder(*llvm_module);

    const char *JIYU_PRODUCER_STRING = "Jiyu Compiler";
    bool is_optimized = false; // @BuildOptions
    const char *COMMAND_LINE_FLAGS = "";
    const unsigned runtime_version = 0;
    di_compile_unit = dib->createCompileUnit(dwarf::DW_LANG_C, DIFile::get(*llvm_context, "fib.jyu", "."), JIYU_PRODUCER_STRING, is_optimized, COMMAND_LINE_FLAGS, runtime_version);

    type_void = Type::getVoidTy(*llvm_context);
    type_i1   = Type::getInt1Ty(*llvm_context);
    type_i8   = Type::getInt8Ty(*llvm_context);
    type_i16  = Type::getInt16Ty(*llvm_context);
    type_i32  = Type::getInt32Ty(*llvm_context);
    type_i64  = Type::getInt64Ty(*llvm_context);

    type_f32  = Type::getFloatTy(*llvm_context);
    type_f64  = Type::getDoubleTy(*llvm_context);
    type_f128 = Type::getFP128Ty(*llvm_context);

    type_string_length = nullptr;
    if (TargetMachine->getPointerSize(0) == 4) {
        type_string_length = type_i32;
        type_intptr = type_i32;
    } else if (TargetMachine->getPointerSize(0) == 8) {
        type_string_length = type_i64;
        type_intptr = type_i64;
    }

    assert(type_string_length);

    // Matches the definition in general.h, except when the target's pointer size doesn't match the host's.
    type_string = StructType::create(*llvm_context, { type_i8->getPointerTo(), type_string_length }, "string", false/*packed*/);

    di_type_bool = dib->createBasicType("bool",    8, dwarf::DW_ATE_boolean);
    di_type_s8   = dib->createBasicType("int8",    8, dwarf::DW_ATE_signed);
    di_type_s16  = dib->createBasicType("int16",  16, dwarf::DW_ATE_signed);
    di_type_s32  = dib->createBasicType("int32",  32, dwarf::DW_ATE_signed);
    di_type_s64  = dib->createBasicType("int64",  64, dwarf::DW_ATE_signed);
    di_type_u8   = dib->createBasicType("uint8",   8, dwarf::DW_ATE_unsigned);
    di_type_u16  = dib->createBasicType("uint16", 16, dwarf::DW_ATE_unsigned);
    di_type_u32  = dib->createBasicType("uint32", 32, dwarf::DW_ATE_unsigned);
    di_type_u64  = dib->createBasicType("uint64", 64, dwarf::DW_ATE_unsigned);
    di_type_f32  = dib->createBasicType("float",  32, dwarf::DW_ATE_float);
    di_type_f64  = dib->createBasicType("double", 64, dwarf::DW_ATE_float);
    di_type_f64  = dib->createBasicType("float128", 128, dwarf::DW_ATE_float);

    di_type_string_length = nullptr;
    if (TargetMachine->getPointerSize(0) == 4) {
        di_type_string_length = di_type_s32;
    } else if (TargetMachine->getPointerSize(0) == 8) {
        di_type_string_length = di_type_s64;
    }

    {
        auto debug_file = DIFile::get(*llvm_context, "", "");
        unsigned line_number = 0;
        DINode::DIFlags flags = DINode::DIFlags();

        auto di_data_type  = dib->createPointerType(di_type_u8, TargetMachine->getPointerSizeInBits(0));

        // @Cleanup literal numbers
        auto data = dib->createMemberType(di_compile_unit, "data", debug_file, line_number,
                            TargetMachine->getPointerSizeInBits(0), TargetMachine->getPointerSizeInBits(0),
                            0, flags, di_data_type);
        auto length = dib->createMemberType(di_compile_unit, "length", debug_file, line_number,
                            type_string_length->getPrimitiveSizeInBits(), type_string_length->getPrimitiveSizeInBits(),
                            TargetMachine->getPointerSizeInBits(0), flags, di_type_string_length);
        auto elements = dib->getOrCreateArray({data, length});

        auto type = compiler->type_string;
        di_type_string = dib->createStructType(di_compile_unit, "string", debug_file,
                            line_number, type->size * BYTES_TO_BITS, type->alignment * BYTES_TO_BITS,
                            flags, nullptr, elements);
    }

    {
        auto debug_file = DIFile::get(*llvm_context, "", "");
        unsigned line_number = 0;
        DINode::DIFlags flags = DINode::DIFlags();

        auto info = compiler->type_info_type;
        auto elements = dib->getOrCreateArray({});
        di_type_type = dib->createStructType(di_compile_unit, "Type", debug_file, line_number,
                        info->size * BYTES_TO_BITS, info->alignment * BYTES_TO_BITS, flags, nullptr, elements);
    }

    di_current_scope = di_compile_unit;

    llvm_types.resize(compiler->type_table.count);

    for (auto entry: compiler->type_table) {
        if (llvm_types[entry->type_table_index]) continue;

        if (types_match(entry, compiler->type_void)) {
            llvm_types[entry->type_table_index] = type_void;
            continue;
        }

        llvm_types[entry->type_table_index] = make_llvm_type(entry);
    }

    // @Incomplete do this for llvm_debug_types as well..
}

void LLVM_Generator::finalize() {
    dib->finalize();

    std::string TargetTriple = TargetMachine->getTargetTriple().str();
    // printf("TRIPLE: %s\n", TargetTriple.c_str());

    llvm_module->setDataLayout(TargetMachine->createDataLayout());
    llvm_module->setTargetTriple(TargetTriple);

    bool is_win32 = TargetMachine->getTargetTriple().isOSWindows();
    if (is_win32) {
        llvm_module->addModuleFlag(Module::Warning, "CodeView", 1);
    }

    String exec_name = compiler->build_options.executable_name;
    String obj_name = mprintf("%.*s.o", PRINT_ARG(exec_name));

    std::error_code EC;
    raw_fd_ostream dest(string_ref(obj_name), EC, sys::fs::F_None);

    if (EC) {
        compiler->report_error((Ast *)nullptr, "Could not open file: %s\n", EC.message().c_str());
        return;
    }

    legacy::PassManager pass;
    auto FileType = TargetMachine::CGFT_ObjectFile;

    /*
    auto fpm = make_unique<legacy::FunctionPassManager>(llvm_module);
    fpm->add(createPromoteMemoryToRegisterPass());
    fpm->doInitialization();

    for (auto &func : llvm_module->functions()) {
        fpm->run(func);
    }
    */

    // llvm_module->dump();
    if (compiler->build_options.emit_llvm_ir) {
        String ll_name = mprintf("%.*s.ll", PRINT_ARG(exec_name));
        raw_fd_ostream ir_stream(string_ref(ll_name), EC, sys::fs::F_None);
        if (EC) {
            compiler->report_error((Ast *)nullptr, "Could not open file: %s\n", EC.message().c_str());
            return;
        }
        llvm_module->print(ir_stream, nullptr);
        free(ll_name.data);
    }

    pass.add(createVerifierPass(false));
    if (TargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
        compiler->report_error((Ast *)nullptr, "TargetMachine can't emit a file of this type"); // @TODO this error message is unclear for the user.
        return;
    }

    pass.run(*llvm_module);
    dest.flush();

    free(obj_name.data);
}

Type *LLVM_Generator::get_type(Ast_Type_Info *type) {
    return llvm_types[type->type_table_index];
}

static bool is_system_v_target(TargetMachine *TM) {
    auto triple = TM->getTargetTriple();
    // @Incomplete there's probably a lot of others that fall into here.
    return triple.isOSLinux() || triple.isMacOSX() || triple.isOSDarwin() || triple.isOSSolaris() || triple.isOSFreeBSD();
}

static bool is_c_return_by_pointer_argument(TargetMachine *TM, Ast_Type_Info *info) {
    info = get_underlying_final_type(info);
    if (!is_aggregate_type(info)) return false;

    bool is_win32 = TM->getTargetTriple().isOSWindows();
    bool is_sysv  = is_system_v_target(TM);

    const int _4BYTES  = 4;
    const int _8BYTES  = 8;
    const int _16BYTES = 16;

    // @TODO is this also true with Thumb?
    if (TM->getTargetTriple().isARM()) {
        if (info->size <= _4BYTES) return false;
        return true;
    }

    // @TODO this probably is incorrect on x86-32 windows.
    if (is_win32 && info->size <= _8BYTES) return false;

    if (is_sysv && info->size <= _16BYTES) return false;

    return true;
}

static bool is_c_pass_by_pointer_argument(TargetMachine *TM, Ast_Type_Info *info) {
    info = get_underlying_final_type(info);
    if (!is_aggregate_type(info)) return false;

    bool is_win32 = TM->getTargetTriple().isOSWindows();
    bool is_sysv  = is_system_v_target(TM);

    // @TODO this probably is incorrect on x86-32 windows.
    const int _8BYTES  = 8;
    const int _16BYTES = 16;

    if (is_win32 && info->size <= _8BYTES) return false;

    if (is_sysv && info->size <= _16BYTES) return false;

    return true;
}

Type *LLVM_Generator::make_llvm_type(Ast_Type_Info *type) {
    type = get_underlying_final_type(type);

    if (type->type == Ast_Type_Info::VOID) {
        // return type_i8 for pointers.
        return type_i8;
    }

    if (type->type == Ast_Type_Info::TYPE) {
        return type_i64;
        // In the future this should return a pointer to the runtime type info.
        //return type_i8->getPointerTo();
    }

    if (type->type == Ast_Type_Info::INTEGER) {
        switch (type->size) {
            case 1: return type_i8;
            case 2: return type_i16;
            case 4: return type_i32;
            case 8: return type_i64;
            default: assert(false);
        }
    }

    if (type->type == Ast_Type_Info::BOOL) {
        return type_i1;
    }

    if (type->type == Ast_Type_Info::FLOAT) {
        switch(type->size) {
            case 4 : return type_f32;
            case 8 : return type_f64;
            case 16: return type_f128;
            default: assert(false);
        }
    }

    if (type->type == Ast_Type_Info::STRING) {
        return type_string;
    }

    if (type->type == Ast_Type_Info::POINTER) {
        auto pointee = make_llvm_type(type->pointer_to);
        return pointee->getPointerTo();
    }

    if (type->type == Ast_Type_Info::ARRAY) {
        auto element = make_llvm_type(type->array_element);
        if (type->array_element_count >= 0) {
            assert(type->is_dynamic == false);

            return ArrayType::get(element, type->array_element_count);
        } else {
            // @Cleanup this should be type_array_count or something
            auto count = type_string_length;
            auto data  = element->getPointerTo();

            if (!type->is_dynamic) {
                return StructType::get(*llvm_context, {data, count}, false);
            } else {
                auto allocated = count;
                return StructType::get(*llvm_context, {data, count, allocated}, false);
            }
        }
    }

    if (type->type == Ast_Type_Info::STRUCT) {
        // Prevent recursion.
        if (llvm_types[type->type_table_index]) {
            return llvm_types[type->type_table_index];
        }

        String name = type->struct_decl->identifier ? type->struct_decl->identifier->name->name : String();
        if (type->struct_decl->is_anonymous) {
            if (type->is_union) {
                name = to_string("union.anon");
            } else {
                name = to_string("struct.anon");
            }
        }

        auto final_type = StructType::create(*llvm_context, string_ref(name));
        llvm_types[type->type_table_index] = final_type;

        Array<Type *> member_types;

        if (!type->is_union) {
            Array<Ast_Type_Info *> flattened_parents;
            flattened_parents.add(type);

            auto parent = type->parent_struct;
            while (parent) {
                parent = get_underlying_final_type(parent);
                flattened_parents.add(parent);
                parent = parent->parent_struct;
            }

            for (array_count_type i = flattened_parents.count; i > 0; --i) {
                auto type = flattened_parents[i-1];

                for (auto member : type->struct_members) {
                    member_types.add(make_llvm_type(member.type_info));
                }
            }
        } else {
            // If the struct is a union, then just use the largest member
            // as the only member, we will bitcast to the right things elsewhere.

            s64 largest_size = -1;
            array_count_type largest_index = -1;

            for (array_count_type i = 0; i < type->struct_members.count; ++i) {
                auto member = type->struct_members[i];
                assert(member.offset_in_struct == 0);

                if (member.type_info->size > largest_size) {
                    largest_size = member.type_info->size;
                    largest_index = i;
                }
            }

            if (largest_index >= 0) {
                auto llvm_type = make_llvm_type(type->struct_members[largest_index].type_info);
                member_types.add(llvm_type);
            }
        }

        final_type->setBody(ArrayRef<Type *>(member_types.data, member_types.count), false/*is packed*/);

        // There doesnt seem to be a way to tell LLVM that this data structure has custom alignment
        // so it's alloc size should technically be slightly larger.
        // if (type->alignment <= 8) {
        //     final_type->dump();
        //     auto size       = TargetMachine->createDataLayout().getTypeSizeInBits(final_type) / 8;
        //     auto alloc_size = TargetMachine->createDataLayout().getTypeAllocSizeInBits(final_type) / 8;
        //     printf("%.*s: %d, alloc %d\n", PRINT_ARG(name), size, alloc_size);
        //     assert(alloc_size == type->stride);
        // }

        // final_type->dump();

        return final_type;
    }

    if (type->type == Ast_Type_Info::ENUM) {
        Type * base_type = make_llvm_type(type->enum_base_type);
        return base_type;
    }

    if (type->type == Ast_Type_Info::FUNCTION) {
        Array<Type *> arguments;

        bool is_c_function = type->is_c_function;

        Type *return_type = make_llvm_type(get_underlying_final_type(type->return_type));
        if (type->return_type->type == Ast_Type_Info::VOID) {
            return_type = type_void;
        }

        // C functions typically return aggregates through a pointer as their first argument.
        // This may not be true depending on the size of the aggregate and the ABI @Incomplete.
        // @Volatile should match functionality in Ast_Function_Call generation.
        bool c_return_is_by_pointer_argument = is_c_function && is_c_return_by_pointer_argument(TargetMachine, type->return_type);
        if (c_return_is_by_pointer_argument) {
            arguments.add(return_type->getPointerTo());
            return_type = type_void;
        }

        for (auto arg_type : type->arguments) {
            arg_type = get_underlying_final_type(arg_type);
            if (arg_type == compiler->type_void) continue;

            Type *type = make_llvm_type(arg_type);

            if (is_c_function) {
                if (is_c_pass_by_pointer_argument(TargetMachine, arg_type)) {
                    arguments.add(type->getPointerTo());
                    continue;
                }
            } else if (is_aggregate_type(arg_type)) {
                arguments.add(type->getPointerTo());
                continue;
            }

            arguments.add(type);
        }

        return FunctionType::get(return_type, ArrayRef<Type *>(arguments.data, arguments.count), type->is_c_varargs)->getPointerTo();
    }

    assert(false);
    return nullptr;
}

DIType *LLVM_Generator::get_debug_type(Ast_Type_Info *type) {
    if (type->type == Ast_Type_Info::ALIAS) {
        // @TODO typedef ?
        return get_debug_type(type->alias_of);
    }

    if (type->type == Ast_Type_Info::VOID) {
        return nullptr;
    }

    if (type->type == Ast_Type_Info::INTEGER) {
        if (type->is_signed) {
            switch (type->size) {
                case 1: return di_type_s8;
                case 2: return di_type_s16;
                case 4: return di_type_s32;
                case 8: return di_type_s64;
                default: assert(false);
            }
        } else {
            switch (type->size) {
                case 1: return di_type_u8;
                case 2: return di_type_u16;
                case 4: return di_type_u32;
                case 8: return di_type_u64;
                default: assert(false);
            }
        }
    }

    if (type->type == Ast_Type_Info::BOOL) {
        return di_type_bool;
    }

    if (type->type == Ast_Type_Info::FLOAT) {
        switch(type->size) {
            case 4: return di_type_f32;
            case 8: return di_type_f64;
            default: assert(false);
        }
    }

    if (type->type == Ast_Type_Info::STRING) {
        return di_type_string;
    }

    if (type->type == Ast_Type_Info::POINTER) {
        auto pointee = get_debug_type(type->pointer_to);
        return dib->createPointerType(pointee, TargetMachine->getPointerSizeInBits(0));
    }

    if (type->type == Ast_Type_Info::ARRAY) {
        auto element = get_debug_type(type->array_element);
        if (type->array_element_count >= 0) {
            assert(type->is_dynamic == false);

            auto subscripts = dib->getOrCreateArray({ dib->getOrCreateSubrange(0, type->array_element_count) });
            return dib->createArrayType(type->size * BYTES_TO_BITS, type->alignment * BYTES_TO_BITS, element, subscripts);
        } else {
            // @Cleanup this should be type_array_count or something
            auto di_count_type = di_type_string_length;
            auto di_data_type  = dib->createPointerType(element, TargetMachine->getPointerSizeInBits(0));

            auto debug_file = DIFile::get(*llvm_context, "", "");

            DINode::DIFlags flags = DINode::DIFlags();

            // @Cleanup literal numbers
            auto data = dib->createMemberType(di_compile_unit, "data", debug_file, 0,
                                TargetMachine->getPointerSizeInBits(0), TargetMachine->getPointerSizeInBits(0),
                                0, flags, di_data_type);
            auto count = dib->createMemberType(di_compile_unit, "count", debug_file, 0,
                                type_string_length->getPrimitiveSizeInBits(), type_string_length->getPrimitiveSizeInBits(),
                                TargetMachine->getPointerSizeInBits(0), flags, di_count_type);

            unsigned line_number = 0;
            DIType *derived_from = nullptr;
            String type_to_string(Ast_Type_Info *info);
            String element_type_string = type_to_string(type->array_element);
            defer { free(element_type_string.data); };
            if (!type->is_dynamic) {
                auto elements = dib->getOrCreateArray({data, count});
                // @Incomplete we should probably be using the correct scope info here.
                String name = mprintf("[] %.*s", PRINT_ARG(element_type_string));
                defer { free(name.data); };

                return dib->createStructType(di_compile_unit, string_ref(name), debug_file,
                                    line_number, type->size * BYTES_TO_BITS, type->alignment * BYTES_TO_BITS,
                                    flags, derived_from, elements);
            } else {
                auto allocated = dib->createMemberType(di_compile_unit, "allocated", debug_file, 0,
                                type_string_length->getPrimitiveSizeInBits(), type_string_length->getPrimitiveSizeInBits(),
                                TargetMachine->getPointerSizeInBits(0) + type_string_length->getPrimitiveSizeInBits(), flags, di_count_type);
                auto elements = dib->getOrCreateArray({data, count, allocated});
                // @Incomplete we should probably be using the correct scope info here.
                String name = mprintf("[..] %.*s", PRINT_ARG(element_type_string));
                defer { free(name.data); };

                return dib->createStructType(di_compile_unit, string_ref(name), debug_file,
                                    line_number, type->size * BYTES_TO_BITS, type->alignment * BYTES_TO_BITS,
                                    flags, derived_from, elements);
            }
        }
    }

    if (type->type == Ast_Type_Info::STRUCT) {
        if (type->debug_type_table_index >= 0) {
            return llvm_debug_types[type->debug_type_table_index];
        }

        auto struct_decl = type->struct_decl;
        auto debug_file = get_debug_file(llvm_context, struct_decl);
        auto line_number = get_line_number(struct_decl);

        String name;
        if (struct_decl->identifier) name = struct_decl->identifier->name->name;
        DIType *derived_from = nullptr;
        DINode::DIFlags flags = DINode::DIFlags();

        DICompositeType *final_type;
        if (!type->is_union) {
            final_type = dib->createStructType(di_compile_unit, string_ref(name), debug_file,
                            line_number, type->size * BYTES_TO_BITS, type->alignment * BYTES_TO_BITS,
                            flags, derived_from, DINodeArray());
        } else {
            final_type = dib->createUnionType(di_compile_unit, string_ref(name), debug_file,
                            line_number, type->size * BYTES_TO_BITS, type->alignment * BYTES_TO_BITS,
                            flags, DINodeArray());
        }

        type->debug_type_table_index = llvm_debug_types.count;
        llvm_debug_types.add(final_type);

        Array<Metadata *> member_types;
        for (auto member : type->struct_members) {
            auto member_type = member.type_info;
            auto di_type = get_debug_type(member_type);

            String name = String();
            if (member.name) name = member.name->name;

            DINode::DIFlags flags = DINode::DIFlags();
            // @Incomplete file and line number are not technically correct.
            auto di_member = dib->createMemberType(di_compile_unit, string_ref(name), debug_file, line_number,
                                        member_type->size * BYTES_TO_BITS, member_type->alignment * BYTES_TO_BITS,
                                        member.offset_in_struct * BYTES_TO_BITS, flags, di_type);
            member_types.add(di_member);
        }

        auto elements = dib->getOrCreateArray(ArrayRef<Metadata *>(member_types.data, member_types.count));
        final_type->replaceElements(elements);

        return final_type;
    }

    if (type->type == Ast_Type_Info::FUNCTION) {
        auto subroutine_type = get_debug_subroutine_type(type);

        // Always return a pointer here. Code that doesn't want a pointer should call get_debug_subroutine_type directly.
        return dib->createPointerType(subroutine_type, TargetMachine->getPointerSizeInBits(0));
    }

    if (type->type == Ast_Type_Info::TYPE) {
        return di_type_type;
    }

    if (type->type == Ast_Type_Info::ENUM) {
        //return get_debug_type(type->enum_base_type);

        auto enum_decl = type->enum_decl;

        String name;
        if (enum_decl->identifier) name = enum_decl->identifier->name->name;

        auto debug_file = get_debug_file(llvm_context, enum_decl);
        auto line_number = get_line_number(enum_decl);

        DIType * base_type = get_debug_type(type->enum_base_type);

        Array<Metadata*> enumerators;
        for (Ast_Expression * element_expr : enum_decl->member_scope.declarations) {
            assert(element_expr->type == AST_DECLARATION);
            auto element_decl = static_cast<Ast_Declaration *>(element_expr);

            assert(element_decl->is_enum_member);
            assert(element_decl->is_let);
            assert(element_decl->initializer_expression != nullptr);

            auto name = element_decl->identifier->name->name;
            Ast_Literal * element_literal = resolves_to_literal_value(element_decl->initializer_expression);
            assert(element_literal->literal_type == Ast_Literal::INTEGER);
            
            enumerators.add(dib->createEnumerator(string_ref(name), element_literal->integer_value));
        }

        DINodeArray elements = dib->getOrCreateArray(ArrayRef<Metadata *>(enumerators.data, enumerators.count));

        // @@ What are UniqueIdentifier and IsScoped for?
        // createEnumerationType(DIScope * Scope, StringRef Name, DIFile * File, unsigned LineNumber,
        //     uint64_t SizeInBits, uint32_t AlignInBits, DINodeArray Elements, DIType * UnderlyingType,
        //     StringRef UniqueIdentifier = "", bool IsScoped = false)

        return dib->createEnumerationType(di_compile_unit, string_ref(name), debug_file, line_number, base_type->getSizeInBits(), base_type->getAlignInBits(), elements, base_type);
    }

    assert(false);
    return nullptr;
}

FunctionType *LLVM_Generator::create_function_type(Ast_Function *function) {
    Type *type = get_type(get_type_info(function));
    assert(type->isPointerTy());

    type = type->getPointerElementType();
    assert(type->isFunctionTy());
    return static_cast<FunctionType *>(type);
}

Value *LLVM_Generator::get_value_for_decl(Ast_Declaration *decl) {
    for (auto &it : decl_value_map) {
        if (it.item1 == decl) {
            return it.item2;
        }
    }

    return nullptr;
}

static Value *create_alloca_in_entry(LLVM_Generator *gen, IRBuilder<> *irb, Ast_Type_Info *type_info) {
    type_info = get_underlying_final_type(type_info);

    auto current_block = irb->GetInsertBlock();
    auto current_debug_loc = irb->getCurrentDebugLocation();

    auto func = current_block->getParent();

    BasicBlock *entry = &func->getEntryBlock();
    irb->SetInsertPoint(entry->getTerminator());

    Type *type = gen->get_type(type_info);
    AllocaInst *alloca = irb->CreateAlloca(type);
    assert(type_info->alignment >= 1);
    alloca->setAlignment(type_info->alignment);

    irb->SetInsertPoint(current_block);
    irb->SetCurrentDebugLocation(current_debug_loc);

    return alloca;
}

Value *LLVM_Generator::create_string_literal(Ast_Literal *lit, bool want_lvalue) {
    assert(lit->literal_type == Ast_Literal::STRING);

    Value *value = nullptr;
    if (lit->string_value.length == 0 || lit->string_value.data == nullptr) {
        value = Constant::getNullValue(type_string);
    } else {
        // null-character automatically inserted by CreateGlobalStringPtr
        Constant *data = irb->CreateGlobalStringPtr(string_ref(lit->string_value));
        Constant *length = ConstantInt::get(type_string_length, lit->string_value.length);

        value = ConstantStruct::get(type_string, { data, length });
    }

    assert(value);

    if (want_lvalue) {
        auto alloca = create_alloca_in_entry(this, irb, compiler->type_string);
        irb->CreateAlignedStore(value, alloca, get_alignment(compiler->type_string));
        return alloca;
    }

    return value;
}

Value *LLVM_Generator::dereference(Value *value, s64 element_path_index, bool is_lvalue) {
    // @TODO I think ideally, the front-end would change and dereferences of constant values with replaecments of literals of the value so that we can simplify LLVM code generation
    if (auto constant = dyn_cast<ConstantAggregate>(value)) {
        return irb->CreateExtractValue(constant, element_path_index);
    } else {
        // @Cleanup type_i32 use for array indexing?
        auto valueptr = irb->CreateGEP(value, { ConstantInt::get(type_i32, 0), ConstantInt::get(type_i32, element_path_index) });
        if (!is_lvalue) return irb->CreateLoad(valueptr);
        return valueptr;
    }
}

// This is different from default_init_struct, because with global variables, we have to
// use a constant-expression, but on the stack, it should generally be better to write to
// the individual fields because the optimizers can better reason about splitting aggregates
// into registers, or something...

// Actually, I've just decided to use get_constant_struct_initializer for default_struct_init
// because the previous method was generating a large amount of store instructions for zero-initialized
// fields (and slowing down compilation by quite a lot). -josh 27 December 2019
Constant *LLVM_Generator::get_constant_struct_initializer(Ast_Type_Info *info) {
    info = get_underlying_final_type(info);

    assert(info->type == Ast_Type_Info::STRUCT);
    assert(info->struct_decl);

    auto llvm_type = static_cast<StructType *>(get_type(info));
    if (info->is_union) {
        return Constant::getNullValue(llvm_type);
    }

    auto _struct = info->struct_decl;

    Array<Constant *> element_values;

    Array<Ast_Struct *> flattened_parents;
    flattened_parents.add(_struct);

    auto parent = info->parent_struct;
    while (parent) {
        parent = get_underlying_final_type(parent);
        flattened_parents.add(parent->struct_decl);

        parent = parent->parent_struct;
    }

    for (array_count_type i = flattened_parents.count; i > 0; --i) {
        auto _struct = flattened_parents[i-1];

        // @Cutnpaste from the stuff below
        for (auto member: _struct->member_scope.declarations) {
            if (member->type == AST_DECLARATION) {
                auto decl = static_cast<Ast_Declaration *>(member);

                if (decl->is_let) continue;
                assert(decl->is_struct_member);

                Ast_Type_Info *member_info = get_type_info(decl);
                Constant *init = nullptr;
                if (decl->initializer_expression) {
                    auto expr = emit_expression(decl->initializer_expression);
                    assert(dyn_cast<Constant>(expr));

                    init = static_cast<Constant *>(expr);
                } else if (is_struct_type(member_info) && !get_underlying_final_type(member_info)->is_union) {
                    init = get_constant_struct_initializer(member_info);
                } else {
                    init = Constant::getNullValue(get_type(member_info));
                }

                assert(init);
                element_values.add(init);
            } else if (member->type == AST_STRUCT) {
                auto struct_decl = static_cast<Ast_Struct *>(member);
                if (struct_decl->is_anonymous) {
                    Constant *init = get_constant_struct_initializer(struct_decl->type_value);
                    assert(init);
                    element_values.add(init);
                }
            }
        }
    }

    return ConstantStruct::get(llvm_type, ArrayRef<Constant *>(element_values.data, element_values.count));
}

void LLVM_Generator::default_init_struct(Value *decl_value, Ast_Type_Info *info) {
    info = get_underlying_final_type(info);

    assert(info->type == Ast_Type_Info::STRUCT);
    assert(info->struct_decl);

    auto _struct = info->struct_decl;

    auto initial_value = get_constant_struct_initializer(info);
    assert(initial_value->getType() == decl_value->getType()->getPointerElementType());
    irb->CreateStore(initial_value, decl_value);

    // s32 element_path_index = 0;
    // for (auto member: _struct->member_scope.declarations) {
    //     if (member->type == AST_DECLARATION) {
    //         auto decl = static_cast<Ast_Declaration *>(member);

    //         if (decl->is_let) continue;
    //         assert(decl->is_struct_member);

    //         if (decl->initializer_expression) {
    //             auto expr = emit_expression(decl->initializer_expression);

    //             auto gep = dereference(decl_value, element_path_index, true);
    //             irb->CreateStore(expr, gep);
    //         } else {
    //             auto mem_info = get_type_info(decl);
    //             if (is_struct_type(mem_info)) {
    //                 auto final_type = get_underlying_final_type(mem_info);
    //                 if (!final_type->is_union) {
    //                     auto gep = dereference(decl_value, element_path_index, true);
    //                     default_init_struct(gep, mem_info);
    //                 }
    //             }
    //         }

    //         element_path_index++;
    //     }
    // }
}

Value *LLVM_Generator::emit_expression(Ast_Expression *expression, bool is_lvalue) {
    while(expression->substitution) expression = expression->substitution;

    switch (expression->type) {
        case AST_SCOPE: {
            auto scope = static_cast<Ast_Scope *>(expression);
            emit_scope(scope);

            return nullptr;
        }

        case AST_SCOPE_EXPANSION: {
            auto exp = static_cast<Ast_Scope_Expansion *>(expression);

            if (exp->expanded_via_import_directive) return nullptr;

            emit_scope(exp->scope);
            return nullptr;
        }

        case AST_UNARY_EXPRESSION: {
            auto un = static_cast<Ast_Unary_Expression *>(expression);

            if (un->operator_type == Token::STAR) {
                auto value = emit_expression(un->expression, true);
                return value;
            } else if (un->operator_type == Token::DEREFERENCE_OR_SHIFT) {
                auto value = emit_expression(un->expression, is_lvalue);

                value = irb->CreateLoad(value);
                return value;
            } else if (un->operator_type == Token::MINUS) {
                auto value = emit_expression(un->expression);
                auto type = get_type_info(un->expression);

                if (is_int_or_enum_type(type)) {
                    return irb->CreateNeg(value);
                } else if (is_float_type(type)) {
                    return irb->CreateFNeg(value);
                }
            } else if (un->operator_type == Token::EXCLAMATION || un->operator_type == Token::TILDE) {
                auto value = emit_expression(un->expression);
                return irb->CreateNot(value);
            }

            assert(false);
            break;
        }

        case AST_BINARY_EXPRESSION: {
            auto bin = static_cast<Ast_Binary_Expression *>(expression);

            if (bin->operator_type == Token::EQUALS) {
                if (bin->left->type == AST_TUPLE_EXPRESSION) {
                    // @Hack I think.
                    // This is needed, otherwise, the tuple-expression will
                    // generate a tuple struct, copy the tuple arguments into
                    // that struct, then return the memory to the struct.
                    // Here we are unpacking the values of the rhs tuple
                    // into the tuple-expression arguments.

                    Value *right = emit_expression(bin->right, false);

                    auto tuple = static_cast<Ast_Tuple_Expression *>(bin->left);
                    for (array_count_type i = 0; i < tuple->arguments.count; ++i) {
                        auto arg = emit_expression(tuple->arguments[i], true);
                        auto value = irb->CreateExtractValue(right, i);

                        irb->CreateStore(value, arg);
                    }

                    return nullptr;
                }

                Value *left  = emit_expression(bin->left,  true);
                Value *right = emit_expression(bin->right, false);

                irb->CreateStore(right, left);
                return nullptr;
            } else {
                Value *left  = emit_expression(bin->left,  false);
                Value *right = emit_expression(bin->right, false);

                // @TODO NUW NSW?
                switch (bin->operator_type) {
                    case Token::STAR: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            return irb->CreateMul(left, right);
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFMul(left, right);
                        }
                    }
                    case Token::PERCENT: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            if (info->is_signed) {
                                return irb->CreateSRem(left, right);
                            } else {
                                return irb->CreateURem(left, right);
                            }
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFRem(left, right);
                        }
                    }
                    case Token::SLASH: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            if (info->is_signed) {
                                return irb->CreateSDiv(left, right);
                            } else {
                                return irb->CreateUDiv(left, right);
                            }
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFDiv(left, right);
                        }
                    }

                    case Token::PLUS: {
                        auto left_type = get_type_info(bin->left);
                        auto right_type = get_type_info(bin->right);

                        if (is_pointer_type(left_type) &&
                            is_int_or_enum_type(right_type)) {
                            return irb->CreateGEP(left, right);
                        } else if (is_pointer_type(left_type) && is_pointer_type(right_type)) {
                            Value *left_int  = irb->CreatePtrToInt(left,  type_intptr);
                            Value *right_int = irb->CreatePtrToInt(right, type_intptr);

                            Value *result = irb->CreateAdd(left_int, right_int);
                            return irb->CreateIntToPtr(result, get_type(left_type));
                        } else if (is_float_type(left_type)) {
                            assert(is_float_type(right_type));

                            return irb->CreateFAdd(left, right);
                        }

                        return irb->CreateAdd(left, right);
                    }
                    case Token::MINUS: {
                        auto left_type  = get_type_info(bin->left);
                        auto right_type = get_type_info(bin->right);

                        if (is_pointer_type(left_type) && is_pointer_type(right_type)) {
                            Value *left_int  = irb->CreatePtrToInt(left,  type_intptr);
                            Value *right_int = irb->CreatePtrToInt(right, type_intptr);

                            Value *result = irb->CreateSub(left_int, right_int);
                            return irb->CreateIntToPtr(result, get_type(left_type));
                        }

                        if (is_float_type(left_type) && is_float_type(right_type)) {
                            return irb->CreateFSub(left, right);
                        }

                        return irb->CreateSub(left, right);
                    }
                    case Token::EQ_OP: {
                        auto info = get_type_info(bin->left);
                        if (is_float_type(info)) {
                            return irb->CreateFCmpUEQ(left, right);
                        } else {
                            return irb->CreateICmpEQ(left, right);
                        }
                    }
                    case Token::NE_OP: {
                        auto info = get_type_info(bin->left);
                        if (is_float_type(info)) {
                            return irb->CreateFCmpUNE(left, right);
                        } else {
                            return irb->CreateICmpNE(left, right);
                        }
                    }
                    case Token::LE_OP: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            if (info->is_signed) {
                                return irb->CreateICmpSLE(left, right);
                            } else {
                                return irb->CreateICmpULE(left, right);
                            }
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFCmpULE(left, right);
                        }
                    }
                    case Token::GE_OP: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            if (info->is_signed) {
                                return irb->CreateICmpSGE(left, right);
                            } else {
                                return irb->CreateICmpUGE(left, right);
                            }
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFCmpUGE(left, right);
                        }
                    }

                    case Token::LEFT_ANGLE: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            if (info->is_signed) {
                                return irb->CreateICmpSLT(left, right);
                            } else {
                                return irb->CreateICmpULT(left, right);
                            }
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFCmpULT(left, right);
                        }
                    }

                    case Token::RIGHT_ANGLE: {
                        auto info = get_type_info(bin->left);
                        if (is_int_or_enum_type(info)) {
                            if (info->is_signed) {
                                return irb->CreateICmpSGT(left, right);
                            } else {
                                return irb->CreateICmpUGT(left, right);
                            }
                        } else {
                            assert(is_float_type(info));
                            return irb->CreateFCmpUGT(left, right);
                        }
                    }

                    case Token::VERTICAL_BAR: {
                        return irb->CreateOr(left, right);
                    }

                    case Token::AMPERSAND: {
                        return irb->CreateAnd(left, right);
                    }

                    case Token::CARET: {
                        return irb->CreateXor(left, right);
                    }

                    case Token::AND_OP: {
                        assert(left->getType()  == type_i1);
                        assert(right->getType() == type_i1);

                        return irb->CreateAnd(left, right);
                    }
                    case Token::OR_OP: {
                        assert(left->getType()  == type_i1);
                        assert(right->getType() == type_i1);

                        return irb->CreateOr(left, right);
                    }

                    case Token::DEREFERENCE_OR_SHIFT: { // <<
                        // This is the integer binary shift.
                        return irb->CreateShl(left, right);
                    }

                    case Token::RIGHT_SHIFT: {
                        auto info = get_type_info(bin->left);

                        if (info->is_signed) {
                            return irb->CreateAShr(left, right);
                        } else {
                            return irb->CreateLShr(left, right);
                        }
                    }
                    default:
                        assert(false && "Unhandled binary expression in emit_expression.");
                        break;
                }
            }

            assert(false);
            break;
        }

        case AST_LITERAL: {
            auto lit = static_cast<Ast_Literal *>(expression);

            auto type_info = get_type_info(lit);
            auto type = get_type(type_info);

            switch (lit->literal_type) {
                case Ast_Literal::STRING:  return create_string_literal(lit, is_lvalue);

                case Ast_Literal::INTEGER: return ConstantInt::get(type, lit->integer_value, type_info->is_signed);
                case Ast_Literal::FLOAT:   return ConstantFP::get(type,  lit->float_value);
                case Ast_Literal::BOOL:    return ConstantInt::get(type, (lit->bool_value ? 1 : 0));
                case Ast_Literal::NULLPTR: return ConstantPointerNull::get(static_cast<PointerType *>(type));
                case Ast_Literal::FUNCTION:return get_or_create_function(lit->function);
            }
        }

        case AST_IDENTIFIER: {
            auto ident = static_cast<Ast_Identifier *>(expression);
            assert(ident->resolved_declaration);

            if (ident->resolved_declaration->type == AST_DECLARATION) {
                auto decl = static_cast<Ast_Declaration *>(ident->resolved_declaration);

                if (decl->is_let && !decl->is_readonly_variable) {
                    return emit_expression(decl->initializer_expression);
                }

                if (decl->identifier && compiler->is_toplevel_scope(decl->identifier->enclosing_scope)) {
                    String name = decl->identifier->name->name;
                    auto value = llvm_module->getNamedGlobal(string_ref(name));
                    assert(value);

                    if (!is_lvalue) return irb->CreateLoad(value);
                    return value;
                }

                auto value = get_value_for_decl(decl);

                if (!is_lvalue) return irb->CreateLoad(value);

                return value;
            } else if (ident->resolved_declaration->type == AST_FUNCTION) {
                auto func = static_cast<Ast_Function *>(ident->resolved_declaration);

                return get_or_create_function(func);
            } else if (is_a_type_declaration(ident->resolved_declaration)) {
                Ast_Type_Info *type_value = get_type_declaration_resolved_type(ident->resolved_declaration);

                // @@ Why not use the table index directly?
                // @Incomplete just stuff the type table index in here for now.. until are able to emit a full type table.
                auto const_int = ConstantInt::get(type_intptr, type_value->type_table_index, true);
                //return ConstantExpr::getIntToPtr(const_int, type_i8->getPointerTo());
                return const_int;
            } else {
                assert(false && "Unhandled identifier type in emit_expression.");
                return nullptr;
            }
        }

        case AST_DECLARATION: {
            auto decl = static_cast<Ast_Declaration *>(expression);

            // Let declarations should have been handled through substitution with the corresponding literal. When invoked 
            // from emit_scope we just skip these declarations, but when invoked from emit_expression this should be an error.
            // Should we use an assert here and skip let declarations in emit_scope?
            //if (decl->is_let && !decl->is_readonly_variable) return nullptr;
            assert(!decl->is_let || decl->is_readonly_variable); 

            auto decl_value = get_value_for_decl(decl);
            if (decl->initializer_expression) {
                auto value = emit_expression(decl->initializer_expression);
                irb->CreateStore(value, decl_value);
            } else {
                // if a declaration does not have an initializer, initialize to 0
                auto type_info = get_type_info(decl);
                if (is_struct_type(type_info) && !get_underlying_final_type(type_info)->is_union) {
                    default_init_struct(decl_value, type_info);
                } else {
                    auto type = decl_value->getType()->getPointerElementType();
                    irb->CreateStore(Constant::getNullValue(type), decl_value);
                }
            }
            return nullptr;
        }

        case AST_FUNCTION_CALL: {
            auto call = static_cast<Ast_Function_Call *>(expression);

            auto type_info = get_underlying_final_type(get_type_info(call->function_or_function_ptr));
            assert(type_info->type == Ast_Type_Info::FUNCTION);

            auto function_target = emit_expression(call->function_or_function_ptr);
            assert(function_target);

            bool is_c_function = type_info->is_c_function;

            bool c_return_is_by_pointer_argument = is_c_function && is_c_return_by_pointer_argument(TargetMachine, type_info->return_type);

            Array<Value *> args;
            if (c_return_is_by_pointer_argument) {
                auto alloca = create_alloca_in_entry(this, irb, type_info->return_type); // Reserve storage for the return value.
                args.add(alloca);
            }

            for (auto &it : call->argument_list) {
                auto info = get_underlying_final_type(get_type_info(it));
                assert(get_size(info) >= 0);

                bool is_pass_by_pointer_aggregate = is_aggregate_type(info);

                if (is_c_function) is_pass_by_pointer_aggregate = is_c_pass_by_pointer_argument(TargetMachine, info);

                auto value = emit_expression(it, is_pass_by_pointer_aggregate);
                args.add(value);
            }

            // promote C vararg arguments if they are not the required sizes
            // for ints, this typically means promoting i8 and i16 to i32.
            if (type_info->is_c_varargs) {
                for (array_count_type i = type_info->arguments.count; i < call->argument_list.count; ++i) {
                    auto value = args[i];
                    auto arg   = call->argument_list[i];

                    auto type = value->getType();
                    if (type->isIntegerTy() && type->getPrimitiveSizeInBits() < type_i32->getPrimitiveSizeInBits()) {
                        auto arg_type = get_underlying_final_type(get_type_info(arg));
                        assert(arg_type->type == Ast_Type_Info::INTEGER || arg_type->type == Ast_Type_Info::BOOL);
                        if (get_type_info(arg)->is_signed) {
                            args[i] = irb->CreateSExt(value, type_i32);
                        } else {
                            args[i] = irb->CreateZExt(value, type_i32);
                        }
                    } else if (type->isFloatTy() && type->getPrimitiveSizeInBits() < type_f64->getPrimitiveSizeInBits()) {
                        assert(is_float_type(get_type_info(arg)));
                        args[i] = irb->CreateFPExt(value, type_f64);
                    }
                }
            }

            Value *result = irb->CreateCall(function_target, ArrayRef<Value *>(args.data, args.count));

            if (c_return_is_by_pointer_argument) {
                result = args[0];
                if (is_lvalue) return result;
                return irb->CreateLoad(result);
            }

            if (is_lvalue) {
                auto alloca = create_alloca_in_entry(this, irb, type_info->return_type);
                irb->CreateStore(result, alloca);
                return alloca;
            }

            return result;
        }

        case AST_DEREFERENCE: {
            auto deref = static_cast<Ast_Dereference *>(expression);
            auto lhs = emit_expression(deref->left, true);

            auto left_expr = deref->left;
            while (left_expr->substitution) left_expr = left_expr->substitution;

            auto left_type = get_underlying_final_type(get_type_info(left_expr));
            bool do_pointer_deref = false;
            if (is_pointer_type(left_type)) {
                // free pointer dereference
                left_type = left_type->pointer_to;
                do_pointer_deref = true;
            }

            if (left_type->type == Ast_Type_Info::STRUCT) {
                // if we're dereferencing a union, bitcast to the right type
                // since LLVM does not have a union type by default.
                if (left_type->is_union) {
                    for (auto member: left_type->struct_members) {
                        if (member.element_index == deref->element_path_index) {
                            auto llvm_type = llvm_types[member.type_info->type_table_index]->getPointerTo();
                            auto bitcast = irb->CreateBitCast(lhs, llvm_type);

                            if (!is_lvalue) return irb->CreateLoad(bitcast);
                            return bitcast;
                        }
                    }
                }
            }


            assert(deref->element_path_index >= 0);

            // @Incomplete
            // assert(deref->byte_offset >= 0);

            if (do_pointer_deref) {
                lhs = irb->CreateLoad(lhs);
            }
            auto value = dereference(lhs, deref->element_path_index, is_lvalue);
            return value;
        }

        case AST_CAST: {
            auto cast = static_cast<Ast_Cast *>(expression);
            Value *value = emit_expression(cast->expression);

            auto src = get_underlying_final_type(get_type_info(cast->expression));
            auto dst = get_underlying_final_type(cast->type_info);

            // When casting, treat enums as if they were integers.
            if (src->type == Ast_Type_Info::ENUM) src = src->enum_base_type;
            if (dst->type == Ast_Type_Info::ENUM) dst = dst->enum_base_type;

            auto dst_type = get_type(dst);
            if (is_int_or_enum_type(src) && is_int_or_enum_type(dst)) {
                if (src->size > dst->size) {
                    return irb->CreateTrunc(value, dst_type);
                } else if (src->size < dst->size) {
                    if (src->is_signed && dst->is_signed) {
                        return irb->CreateSExt(value, dst_type);
                    } else {
                        return irb->CreateZExt(value, dst_type);
                    }
                }

                assert(get_type(src) == dst_type);
                return value;
            } else if (is_float_type(src) && is_float_type(dst)) {
                if (src->size < dst->size) {
                    return irb->CreateFPExt(value, dst_type);
                } else if (src->size > dst->size) {
                    return irb->CreateFPTrunc(value, dst_type);
                }

                assert(get_type(src) == dst_type);
                return value;
            } else if (is_float_type(src) && is_int_type(dst)) {
                if (dst->is_signed) {
                    return irb->CreateFPToSI(value, dst_type);
                } else {
                    return irb->CreateFPToUI(value, dst_type);
                }
            } else if (is_int_type(src) && is_float_type(dst)) {
                if (src->is_signed) {
                    return irb->CreateSIToFP(value, dst_type);
                } else {
                    return irb->CreateUIToFP(value, dst_type);
                }
            } else if (is_pointer_type(src) && is_pointer_type(dst)) {
                return irb->CreatePointerCast(value, dst_type);
            } else if (is_function_type(src) && is_function_type(dst)) {
                return irb->CreatePointerCast(value, dst_type);
            } else if (is_pointer_type(src) && is_int_type(dst)) {
                return irb->CreatePtrToInt(value, dst_type);
            } else if (is_int_type(src) && is_pointer_type(dst)) {
                return irb->CreateIntToPtr(value, dst_type);
            } else if (is_pointer_type(src) && dst->type == Ast_Type_Info::FUNCTION) {
                return irb->CreatePointerCast(value, dst_type);
            }

            assert(false);
            break;
        }

        case AST_IF: {
            auto _if = static_cast<Ast_If *>(expression);

            auto cond = emit_expression(_if->condition);

            auto current_block = irb->GetInsertBlock();
            auto current_debug_location = irb->getCurrentDebugLocation();

            BasicBlock *then_block = BasicBlock::Create(*llvm_context, "then_target", current_block->getParent());
            BasicBlock *else_block = nullptr;
            if (_if->else_scope) else_block = BasicBlock::Create(*llvm_context, "else_target", current_block->getParent());
            BasicBlock *next_block = BasicBlock::Create(*llvm_context, "", current_block->getParent());
            BasicBlock *failure_target = else_block ? else_block : next_block;

            irb->CreateCondBr(cond, then_block, failure_target);

            irb->SetInsertPoint(then_block);
            emit_scope(&_if->then_scope);
            if (!irb->GetInsertBlock()->getTerminator()) irb->CreateBr(next_block);


            if (_if->else_scope) {
                irb->SetInsertPoint(else_block);
                emit_scope(_if->else_scope);

                if (!irb->GetInsertBlock()->getTerminator()) irb->CreateBr(next_block);

                failure_target = else_block;
            }

            irb->SetInsertPoint(next_block);
            break;
        }

        case AST_WHILE: {
            auto loop = static_cast<Ast_While *>(expression);

            auto current_block = irb->GetInsertBlock();

            BasicBlock *next_block = BasicBlock::Create(*llvm_context, "loop_exit", current_block->getParent());
            BasicBlock *loop_header = BasicBlock::Create(*llvm_context, "loop_header", current_block->getParent());
            BasicBlock *loop_body = BasicBlock::Create(*llvm_context, "loop_body", current_block->getParent());

            loop_header_map.add(MakeTuple(static_cast<Ast_Expression *>(loop), loop_header));
            loop_exit_map.add(MakeTuple(static_cast<Ast_Expression *>(loop), next_block));


            irb->CreateBr(loop_header);

            irb->SetInsertPoint(loop_header);
            irb->SetCurrentDebugLocation(DebugLoc::get(get_line_number(loop->condition), 0, di_current_scope));
            // emit the condition in the loop header so that it always executes when we loop back around
            auto cond = emit_expression(loop->condition);
            irb->SetInsertPoint(loop_header);
            irb->CreateCondBr(cond, loop_body, next_block);

            irb->SetInsertPoint(loop_body);
            {
                emit_scope(&loop->body);
                // irb->SetInsertPoint(loop_body);
                if (!irb->GetInsertBlock()->getTerminator()) irb->CreateBr(loop_header);
            }

            auto current_debug_loc = irb->getCurrentDebugLocation();
            irb->SetInsertPoint(next_block);
            irb->SetCurrentDebugLocation(current_debug_loc);
            break;
        }

        case AST_FOR: {
            auto _for = static_cast<Ast_For *>(expression);

            auto it_decl = _for->iterator_decl;
            auto it_alloca = create_alloca_in_entry(this, irb, get_type_info(it_decl));
            auto decl_type = get_type_info(it_decl);

            decl_value_map.add(MakeTuple(it_decl, it_alloca));

            auto it_index_decl = _for->iterator_index_decl;
            Ast_Type_Info *it_index_type = nullptr;
            Value *it_index_alloca = nullptr;
            if (it_index_decl) {
                it_index_type = get_type_info(it_index_decl);
                it_index_alloca = create_alloca_in_entry(this, irb, it_index_type);
                decl_value_map.add(MakeTuple(it_index_decl, it_index_alloca));
                emit_expression(it_index_decl);
            } else {
                it_index_type = decl_type;
                it_index_alloca = it_alloca;

                emit_expression(it_decl);
            }

            auto current_block = irb->GetInsertBlock();

            BasicBlock *loop_header = BasicBlock::Create(*llvm_context, "for_header", current_block->getParent());
            BasicBlock *loop_body = BasicBlock::Create(*llvm_context, "for_body", current_block->getParent());

            // Where the iterator increment happens. This is factored out to be the target of _continue_ statements.
            BasicBlock *loop_body_end = BasicBlock::Create(*llvm_context, "for_body_end", current_block->getParent());
            loop_header_map.add(MakeTuple(static_cast<Ast_Expression *>(_for), loop_body_end));

            BasicBlock *next_block = BasicBlock::Create(*llvm_context, "for_exit", current_block->getParent());
            loop_exit_map.add(MakeTuple(static_cast<Ast_Expression *>(_for), next_block));

            irb->CreateBr(loop_header);
            irb->SetInsertPoint(loop_header);
            // emit the condition in the loop header so that it always executes when we loop back around
            auto it_index = irb->CreateLoad(it_index_alloca);
            assert(is_int_type(it_index_type));

            auto upper = emit_expression(_for->upper_range_expression);
            Value *cond = nullptr;
            if (it_index_decl || _for->is_exclusive_end) {
                // use < here otherwise, we'll overstep by one.
                // @Cleanup maybe this should be flagged as a half-open loop
                // when we support that?
                if (it_index_type->is_signed) {
                    cond = irb->CreateICmpSLT(it_index, upper);
                } else {
                    cond = irb->CreateICmpULT(it_index, upper);
                }
            } else {
                if (it_index_type->is_signed) {
                    cond = irb->CreateICmpSLE(it_index, upper);
                } else {
                    cond = irb->CreateICmpULE(it_index, upper);
                }
            }

            irb->SetInsertPoint(loop_header);
            irb->CreateCondBr(cond, loop_body, next_block);

            irb->SetInsertPoint(loop_body);
            if (it_index_decl) {
                emit_expression(it_decl);
            }

            emit_scope(&_for->body);

            if (!irb->GetInsertBlock()->getTerminator()) irb->CreateBr(loop_body_end);

            irb->SetInsertPoint(loop_body_end);
            irb->CreateStore(irb->CreateAdd(it_index, ConstantInt::get(get_type(it_index_type), 1)), it_index_alloca);
            irb->CreateBr(loop_header);

            irb->SetInsertPoint(next_block);
            break;
        }

        case AST_RETURN: {
            auto ret = static_cast<Ast_Return *>(expression);
            if (ret->expression) {
                auto value = emit_expression(ret->expression);
                assert(value);
                irb->CreateRet(value);
            } else {
                irb->CreateRetVoid();
            }

            // Create a new block so subsequent instructions have some where to generate to
            // @TODO Actually, Idk if this is correct, will have to test with how ifs and loops work...

            /*
            auto current_block = irb->GetInsertBlock();
            BasicBlock *new_block = BasicBlock::Create(*llvm_context, "", current_block->getParent());

            irb->SetInsertPoint(new_block);
            */
            break;
        }

        case AST_ARRAY_DEREFERENCE: {
            auto deref = static_cast<Ast_Array_Dereference *>(expression);

            auto array = emit_expression(deref->array_or_pointer_expression, true);
            auto index = emit_expression(deref->index_expression);

            auto type = get_type_info(deref->array_or_pointer_expression);
            type = get_underlying_final_type(type);

            if (type->type == Ast_Type_Info::ARRAY && type->array_element_count == -1) {
                // @Cleanup hardcoded indices
                array = irb->CreateGEP(array, {ConstantInt::get(type_i32, 0), ConstantInt::get(type_i32, 0)});
                array = irb->CreateLoad(array);
                auto element = irb->CreateGEP(array, index);

                if (!is_lvalue) return irb->CreateLoad(element);
                return element;
            } else if (type->type == Ast_Type_Info::STRING) {
                // @Note although this is identical to the dynamic/static array case,
                // I've chosen to duplicate the code in case we chnage the order of
                // any of these implicit struct fields.
                // @Cleanup hardcoded indices.
                array = irb->CreateGEP(array, {ConstantInt::get(type_i32, 0), ConstantInt::get(type_i32, 0)});
                array = irb->CreateLoad(array);
                auto element = irb->CreateGEP(array, index);

                if (!is_lvalue) return irb->CreateLoad(element);
                return element;
            } else if (type->type == Ast_Type_Info::POINTER) {
                auto ptr = irb->CreateLoad(array);
                auto element = irb->CreateGEP(ptr, index);

                if (!is_lvalue) return irb->CreateLoad(element);
                return element;
            }

            // @Cleanup type_i32 use for array indexing
            auto element = irb->CreateGEP(array, {ConstantInt::get(type_i32, 0), index});

            if (!is_lvalue) return irb->CreateLoad(element);
            return element;
        }

        case AST_FUNCTION: {
            auto func = static_cast<Ast_Function *>(expression);

            if (func->is_template_function) {
                // we should not get here if this was an expression use of a function
                return nullptr;
            }

            // we only need the header to be generated when we come here, only the compiler instance can choose to emit a function.
            return get_or_create_function(func);
        }

        case AST_CONTROL_FLOW: {
            auto flow = static_cast<Ast_Control_Flow *>(expression);

            if (flow->control_type == Token::KEYWORD_BREAK) {
                for (auto &entry : loop_exit_map) {
                    if (entry.item1 == flow->target_statement) return irb->CreateBr(entry.item2);
                }
            } else if (flow->control_type == Token::KEYWORD_CONTINUE) {
                for (auto &entry : loop_header_map) {
                    if (entry.item1 == flow->target_statement) return irb->CreateBr(entry.item2);
                }
            }

            assert(false && "Could not find LLVM BasicBlock for control-flow statement.");
            return nullptr;
        }

        case AST_TUPLE_EXPRESSION: {
            auto tuple = static_cast<Ast_Tuple_Expression *>(expression);

            auto memory = create_alloca_in_entry(this, irb, get_type_info(tuple));

            for (array_count_type i = 0; i < tuple->arguments.count; ++i) {
                auto value = emit_expression(tuple->arguments[i]);

                irb->CreateStore(value, dereference(memory, i, true));
            }

            if (is_lvalue) return memory;

            return irb->CreateLoad(memory);
        }

        case AST_SWITCH: {
            auto _switch = static_cast<Ast_Switch *>(expression);

            auto cond = emit_expression(_switch->condition);

            auto current_block = irb->GetInsertBlock();
            auto current_debug_location = irb->getCurrentDebugLocation();

            BasicBlock *next_block = BasicBlock::Create(*llvm_context, "", current_block->getParent());

            loop_exit_map.add(MakeTuple(static_cast<Ast_Expression *>(_switch), next_block));

            auto swinst = irb->CreateSwitch(cond, next_block);

            for (auto stmt : _switch->scope.statements) {
                if (stmt->type != AST_CASE) continue; // @FixMe this doesnt account for Ast_Scope_Expansion

                auto _case = static_cast<Ast_Case *>(stmt);

                BasicBlock *case_block = static_cast<BasicBlock *>(emit_expression(_case));

                for (auto case_cond: _case->conditions) {
                    auto value = emit_expression(case_cond);
                    assert(dyn_cast<ConstantInt>(value));

                    swinst->addCase(static_cast<ConstantInt *>(value), case_block);
                }
            }


            irb->SetInsertPoint(next_block);

            return nullptr;
        }

        case AST_CASE: {
            auto _case = static_cast<Ast_Case *>(expression);

            // Case works different to the rest of the control-flow constructs in that we are only here to generate
            // the contents of its scope into a block and then returning those blocks as values to Ast_Switch.
            // Ast_Switch will handle generating the case conditions.

            auto current_block = irb->GetInsertBlock();
            auto current_debug_location = irb->getCurrentDebugLocation();

            BasicBlock *block = BasicBlock::Create(*llvm_context, "case_block", current_block->getParent());
            irb->SetInsertPoint(block);
            emit_scope(&_case->scope);
            if (!irb->GetInsertBlock()->getTerminator()) {
                for (auto &entry : loop_exit_map) {
                    if (entry.item1 == _case->target_switch) {
                        irb->CreateBr(entry.item2);
                        break;
                    }
                }

                assert(irb->GetInsertBlock()->getTerminator());
            }

            irb->SetInsertPoint(current_block);
            irb->SetCurrentDebugLocation(current_debug_location);
            return block;
        }

        case AST_UNINITIALIZED:
            assert(false && "Unitialized AST Node!");
        // No-ops
        case AST_TYPE_ALIAS:
        case AST_STRUCT:
        case AST_ENUM:
        case AST_DIRECTIVE_LOAD:
        case AST_DIRECTIVE_IMPORT:
        case AST_DIRECTIVE_STATIC_IF:
        case AST_DIRECTIVE_CLANG_IMPORT:
        case AST_LIBRARY:
            break;
        case AST_TYPE_INSTANTIATION:
        case AST_OS:      // This is always subtituted by a literal at the AST level.
        case AST_SIZEOF:  // This is always subtituted by a literal at the AST level.
        case AST_TYPEOF:  // This is always subtituted by a literal at the AST level.
        case AST_DEFINED: // This is always subtituted by a literal at the AST level.
            assert(false);
            break;
    }

    return nullptr;
}

Function *LLVM_Generator::get_or_create_function(Ast_Function *function) {

    if (function->is_intrinsic) {
        if (function->identifier->name == compiler->atom_builtin_debugtrap) {
            return Intrinsic::getDeclaration(llvm_module, Intrinsic::debugtrap);
        }

        assert(false);
        return nullptr;
    }

    assert(function->identifier);
    String linkage_name = function->linkage_name;

    auto func = llvm_module->getFunction(string_ref(linkage_name));

    if (!func) {
        FunctionType *function_type = create_function_type(function);
        auto linkage = GlobalValue::LinkageTypes::ExternalLinkage;
        if (!function->is_c_function && !function->is_exported) {
            linkage = GlobalValue::LinkageTypes::InternalLinkage;
        }

        func = Function::Create(function_type, linkage, string_ref(linkage_name), llvm_module);

        array_count_type i = 0;
        for (auto &a : func->args()) {
            if (i < function->arguments.count) {
                a.setName(string_ref(function->arguments[i]->identifier->name->name));

                ++i;
            }
        }
    }

    return func;
}

void LLVM_Generator::emit_scope(Ast_Scope *scope) {
    auto old_di_scope = di_current_scope;
    di_current_scope = dib->createLexicalBlock(old_di_scope, get_debug_file(llvm_context, scope), get_line_number(scope), 0);

    auto current_block = irb->GetInsertBlock();
    auto func = current_block->getParent();
    BasicBlock *entry_block = &func->getEntryBlock();

    // setup variable mappings
    for (auto it : scope->declarations) {
        assert(it->substitution == nullptr);
        // while (it->substitution) it = it->substitution;

        if (it->type != AST_DECLARATION) continue;

        auto decl = static_cast<Ast_Declaration *>(it);
        if (decl->is_let && !decl->is_readonly_variable) continue;

        auto alloca = create_alloca_in_entry(this, irb, get_type_info(it));

        String name;
        if (decl->identifier) name = decl->identifier->name->name;

        if (decl->identifier) {
            alloca->setName(string_ref(name));
        }

        assert(get_value_for_decl(decl) == nullptr);
        decl_value_map.add(MakeTuple(decl, alloca));

        // debug info

        // @TODO this should be based on desired optimization. Though, in my experience,
        // even this flag doesn't help preserve the actual stack variable much on Windows
        // without using a hack like inserting a GEP. -josh 18 August 2019
        bool always_preserve = true;
        auto di_type = get_debug_type(get_type_info(it));
        auto di_local_var = dib->createAutoVariable(di_current_scope, string_ref(name),
                                get_debug_file(llvm_context, it), get_line_number(it), di_type, always_preserve);
        dib->insertDeclare(alloca, di_local_var, DIExpression::get(*llvm_context, None), DebugLoc::get(get_line_number(it), 0, di_current_scope),
                            current_block);
    }

    for (auto &it : scope->statements) {
        if (it->type == AST_DECLARATION) {
            auto decl = static_cast<Ast_Declaration *>(it);
            if (decl->is_let && !decl->is_readonly_variable) continue;
        }

        irb->SetCurrentDebugLocation(DebugLoc::get(get_line_number(it), 0, di_current_scope));
        emit_expression(it);
    }

    di_current_scope = old_di_scope;
}

DISubroutineType *LLVM_Generator::get_debug_subroutine_type(Ast_Type_Info *type) {
    Array<Metadata *> arguments;
    DIType *return_type = get_debug_type(type->return_type);
    // @Incomplete void return types need to be null?

    arguments.add(return_type);

//     bool is_c_function = type->is_c_function;
//     bool is_win32 = TargetMachine->getTargetTriple().isOSWindows();

    for (auto arg_type : type->arguments) {
        if (arg_type == compiler->type_void) continue;

        DIType *di_type = get_debug_type(arg_type);

        // if (is_c_function && is_win32 && is_aggregate_type(arg_type)) {
        //     assert(arg_type->size >= 0);

        //     // @TargetInfo this is only true for x64 too
        //     const int _8BYTES = 8;
        //     if (arg_type->size > _8BYTES) {
        //         arguments.add(type->getPointerTo());
        //         continue;
        //     }
        // }

        if (is_aggregate_type(arg_type)) {
            di_type = dib->createReferenceType(dwarf::DW_TAG_reference_type, di_type, TargetMachine->getPointerSizeInBits(0));
        }

        arguments.add(di_type);
    }

    return dib->createSubroutineType(dib->getOrCreateTypeArray(ArrayRef<Metadata *>(arguments.data, arguments.count)));
}

void LLVM_Generator::emit_function(Ast_Function *function) {
    assert(function->identifier && function->identifier->name);
    if (!function->scope) return;

    Function *func = get_or_create_function(function);

    {
        func->addFnAttr(Attribute::AttrKind::NoUnwind);

        // Unwind tables are necessary on Windows regardless if we support exceptions or not
        // in order for stack unwinding to work in debuggers, and probably for SEH too. This
        // doesnt seem to be necessary on Unix-style platforms. -josh 15 December 2019
        if (TargetMachine->getTargetTriple().isOSWindows()) {
            func->addFnAttr(Attribute::AttrKind::UWTable);
        }
    }

    if (!function->scope) return; // forward declaration of external thing

    if (!func->empty()) {
        String name = function->linkage_name;
        compiler->report_error(function, "Function with linkage name \"%.*s\" already has been defined!\n", name.length, name.data);
        return;
    }

    StringRef function_name = string_ref(function->identifier->name->name);
    StringRef linkage_name  = string_ref(function->linkage_name);
    auto subroutine_type    = get_debug_subroutine_type(get_type_info(function));
    assert(di_current_scope);
    auto di_subprogram      = dib->createFunction(di_current_scope, function_name, linkage_name,
                                            get_debug_file(llvm_context, function), get_line_number(function),
                                            subroutine_type, get_line_number(function->scope), DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
    func->setSubprogram(di_subprogram);

    auto old_di_scope = di_current_scope;
    di_current_scope = di_subprogram;

    // create entry block
    BasicBlock *entry = BasicBlock::Create(*llvm_context, "entry", func);
    BasicBlock *starting_block = BasicBlock::Create(*llvm_context, "start", func);

    irb->SetInsertPoint(entry);
    irb->SetCurrentDebugLocation(DebugLoc::get(get_line_number(function), 0, di_current_scope));

    auto arg_it = func->arg_begin();
    for (array_count_type i = 0; i < function->arguments.count; ++i) {
        auto a  = arg_it;

        auto decl = function->arguments[i];

        Value *storage = nullptr;
        if (is_aggregate_type(get_type_info(decl))) {
            // Aggregate parameters are already references/pointers so we don't need storage for them.
            assert(get_value_for_decl(decl) == nullptr);
            decl_value_map.add(MakeTuple(decl, static_cast<Value *>(a)));

            storage = a;
        } else {
            // Create storage for value-parameters so that we can treat the parameters
            // the same as local variables during code generation. Maybe this isn't super necessary anymore, idk!
            AllocaInst *alloca = irb->CreateAlloca(get_type(get_type_info(decl)));
            assert(get_alignment(get_type_info(decl)) >= 1);
            alloca->setAlignment(get_alignment(get_type_info(decl)));
            irb->CreateStore(a, alloca);

            assert(get_value_for_decl(decl) == nullptr);
            decl_value_map.add(MakeTuple<Ast_Declaration *, Value *>(decl, alloca));

            storage = alloca;
        }

        String name;
        if (decl->identifier) name = decl->identifier->name->name;

        auto di_type = get_debug_type(get_type_info(decl));
        bool always_preserve = true; // @TODO should be based on desired optimization.
        auto param = dib->createParameterVariable(di_subprogram, string_ref(name), i+1,
                            get_debug_file(llvm_context, decl), get_line_number(decl),
                            di_type, always_preserve);
        dib->insertDeclare(storage, param, DIExpression::get(*llvm_context, None), DebugLoc::get(get_line_number(decl), 0, di_subprogram),
                            starting_block);

        arg_it++;
    }

    irb->CreateBr(starting_block);

    irb->SetInsertPoint(starting_block);
    emit_scope(function->scope);

    auto current_block = irb->GetInsertBlock();

    if (!current_block->getTerminator()) {
        auto return_type = function->return_type;
        // @Cleanup early out for void types since we use i8 for pointers
        if (return_type && get_underlying_final_type(return_type->type_value)->type != Ast_Type_Info::VOID) {
            irb->CreateRet(Constant::getNullValue(get_type(return_type->type_value)));
        } else {
            irb->CreateRetVoid();
        }
    }

    decl_value_map.clear();
    loop_header_map.clear();
    loop_exit_map.clear();

    di_current_scope = old_di_scope;
}

void LLVM_Generator::emit_global_variable(Ast_Declaration *decl) {
    bool is_constant = false;
    String name = decl->identifier->name->name;
    Type *type = get_type(get_type_info(decl));

    Constant *const_init = nullptr;
    if (decl->initializer_expression) {
        auto init = emit_expression(decl->initializer_expression);
        const_init = dyn_cast<llvm::Constant>(init);
        assert(const_init);
    } else if (is_struct_type(get_type_info(decl))) {
        // If this is a struct type, and maybe @TODO Tuples too, then
        // we need to build a constant-initializer of the default values
        // of the struct fields.

        const_init = get_constant_struct_initializer(get_type_info(decl));
    } else {
        const_init = Constant::getNullValue(type);
    }

    auto GV = new GlobalVariable(*llvm_module, type, is_constant, GlobalVariable::InternalLinkage, const_init, string_ref(name));
    UNUSED(GV, "LLVM internally manages this, just marking unused to silence warning.");
}

#include <stdio.h>

void LLVM_Jitter::init() {
    Triple target_triple  = llvm->TargetMachine->getTargetTriple();
    Triple process_triple = Triple(llvm::sys::getProcessTriple());

    if (!target_triple.isCompatibleWith(process_triple)) {
        compiler->report_error((Ast *)nullptr, "Metaprogram target triple (%s) must be compatible with host process' target (%s).",
                    target_triple.str().c_str(), process_triple.str().c_str());
        return;
    }

    for (auto lib: compiler->libraries) {
        String name = lib->libname;
        if (name == to_string("jiyu")) continue; // @Hack @Temporary loading the jiyu DLL causes some issues with LLVM on Linux.

        auto c_str = to_c_string(name);
        if (!lib->is_framework) {
            bool not_valid = llvm::sys::DynamicLibrary::LoadLibraryPermanently(c_str);
            if (not_valid) { // It cannot be loaded by name alone so it may not be a system library. Try provided search paths instead.
                for (auto path: compiler->library_search_paths) {

                    String fullpath;
                    if (llvm->TargetMachine->getTargetTriple().isOSWindows()) {
                        fullpath = mprintf("%.*s" PATH_SEPARATOR "%s", path.length, path.data, c_str);
                    } else {
                        char *ext = "so";
                        if (llvm->TargetMachine->getTargetTriple().isMacOSX()) ext = "dylib";

                        fullpath = mprintf("%.*s" PATH_SEPARATOR "lib%s.%s", path.length, path.data, c_str, ext);
                    }
                    auto fullpath_c_string = to_c_string(fullpath);
                    // printf("PATH: %s\n", fullpath_c_string);
                    not_valid = llvm::sys::DynamicLibrary::LoadLibraryPermanently(fullpath_c_string);
                    free(fullpath.data);
                    free(fullpath_c_string);
                    if (!not_valid) break;
                }
            }
        } else {
            auto fullpath = mprintf("/System/Library/Frameworks" PATH_SEPARATOR "%s.framework" PATH_SEPARATOR "%s", c_str, c_str);
            auto fullpath_c_string = to_c_string(fullpath);
            llvm::sys::DynamicLibrary::LoadLibraryPermanently(fullpath_c_string);
            free(fullpath.data);
            free(fullpath_c_string);
        }

        free(c_str);
    }

    auto JTMB = JITTargetMachineBuilder::detectHost();

    if (!JTMB) {
        JTMB.takeError();
        return;
    }

    auto DL = JTMB->getDefaultDataLayoutForTarget();
    if (!DL) {
        DL.takeError();
        return;
    }

    ES = new ExecutionSession();
    ObjectLayer = new RTDyldObjectLinkingLayer(*ES, []() { return llvm::make_unique<SectionMemoryManager>(); });

#ifdef WIN32
    ObjectLayer->setOverrideObjectFlagsWithResponsibilityFlags(true);

    // Sigh, I dont know why but setting this works around an LLVM bug that trips this assert on Windows:
    // "Resolving symbol outside this responsibility set"
    // There's a Stack Overflow thread discussing the issue here:
    // https://stackoverflow.com/questions/57733912/llvm-asserts-resolving-symbol-outside-this-responsibility-set#comment101934807_57733912
    // For now, the jiyu-game project runs as a metaprogram with this flag set.
    // -josh 18 November 2019
    ObjectLayer->setAutoClaimResponsibilityForObjectSymbols(true);
#endif

    CompileLayer = new IRCompileLayer(*ES, *ObjectLayer, ConcurrentIRCompiler(*JTMB));

    ES->getMainJITDylib().setGenerator(cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(DL->getGlobalPrefix())));

    llvm->dib->finalize();

    llvm->llvm_module->setDataLayout(*DL);
    // llvm->llvm_module->dump();

#ifdef DEBUG
    legacy::PassManager pass;
    pass.add(createVerifierPass(false));
    pass.run(*llvm->llvm_module);
#endif

    cantFail(CompileLayer->add(ES->getMainJITDylib(),
                              ThreadSafeModule(std::unique_ptr<Module>(llvm->llvm_module), *llvm->thread_safe_context)));
}

void *LLVM_Jitter::lookup_symbol(String name) {
    auto JTMB = JITTargetMachineBuilder::detectHost();

    if (!JTMB) {
        JTMB.takeError();
        return nullptr;
    }

    auto DL = JTMB->getDefaultDataLayoutForTarget();
    if (!DL) {
        DL.takeError();
        return nullptr;
    }

    // We're contructing a new MangleAndInterner every time because it seems if you re-use one that was allocated on the heap,
    // sometimes it just crashes...
    MangleAndInterner Mangle(*ES, *DL);
    auto sym = ES->lookup({&ES->getMainJITDylib()}, Mangle(string_ref(name)));
    if (!sym) {
        sym.takeError();
        return nullptr;
    }
    return reinterpret_cast<void *>(sym->getAddress());
}
