#include <Interpreters/ExpressionJIT.h>

#if USE_EMBEDDED_COMPILER

#include <optional>
#include <stack>

#include <common/logger_useful.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnVector.h>
#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionsComparison.h>
#include <DataTypes/Native.h>
#include <Functions/IFunctionAdaptors.h>

#include <Interpreters/JIT/CHJIT.h>
#include <Interpreters/JIT/CompileDAG.h>
#include <Interpreters/JIT/compileFunction.h>
#include <Interpreters/ActionsDAG.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

static CHJIT & getJITInstance()
{
    static CHJIT jit;
    return jit;
}

static Poco::Logger * getLogger()
{
    static Poco::Logger & logger = Poco::Logger::get("ExpressionJIT");
    return &logger;
}

class CompiledFunction
{
public:

    CompiledFunction(void * compiled_function_, CHJIT::CompiledModuleInfo module_info_)
        : compiled_function(compiled_function_)
        , module_info(std::move(module_info_))
    {}

    void * getCompiledFunction() const { return compiled_function; }

    ~CompiledFunction()
    {
        getJITInstance().deleteCompiledModule(module_info);
    }

private:

    void * compiled_function;

    CHJIT::CompiledModuleInfo module_info;
};

class LLVMExecutableFunction : public IExecutableFunction
{
public:

    explicit LLVMExecutableFunction(const std::string & name_, std::shared_ptr<CompiledFunction> compiled_function_)
        : name(name_)
        , compiled_function(compiled_function_)
    {
    }

    String getName() const override { return name; }

    bool useDefaultImplementationForNulls() const override { return false; }

    bool useDefaultImplementationForConstants() const override { return true; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        if (!canBeNativeType(*result_type))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "LLVMExecutableFunction unexpected result type in: {}", result_type->getName());

        auto result_column = result_type->createColumn();

        if (input_rows_count)
        {
            result_column = result_column->cloneResized(input_rows_count);

            std::vector<ColumnData> columns(arguments.size() + 1);
            std::vector<ColumnPtr> columns_backup;

            for (size_t i = 0; i < arguments.size(); ++i)
            {
                auto column = arguments[i].column->convertToFullColumnIfConst();
                columns_backup.emplace_back(column);
                columns[i] = getColumnData(column.get());
            }

            columns[arguments.size()] = getColumnData(result_column.get());

            JITCompiledFunction jit_compiled_function_typed = reinterpret_cast<JITCompiledFunction>(compiled_function->getCompiledFunction());
            jit_compiled_function_typed(input_rows_count, columns.data());

            #if defined(MEMORY_SANITIZER)
            /// Memory sanitizer don't know about stores from JIT-ed code.
            /// But maybe we can generate this code with MSan instrumentation?

            if (const auto * nullable_column = typeid_cast<const ColumnNullable *>(result_column.get()))
            {
                const auto & nested_column = nullable_column->getNestedColumn();
                const auto & null_map_column = nullable_column->getNullMapColumn();

                auto nested_column_raw_data = nested_column.getRawData();
                __msan_unpoison(nested_column_raw_data.data, nested_column_raw_data.size);

                auto null_map_column_raw_data = null_map_column.getRawData();
                __msan_unpoison(null_map_column_raw_data.data, null_map_column_raw_data.size);
            }
            else
            {
                __msan_unpoison(result_column->getRawData().data, result_column->getRawData().size);
            }

            #endif
        }

        return result_column;
    }

private:
    std::string name;
    std::shared_ptr<CompiledFunction> compiled_function;
};

class LLVMFunction : public IFunctionBase
{
public:

    explicit LLVMFunction(const CompileDAG & dag_)
        : name(dag_.dump())
        , dag(dag_)
    {
        for (size_t i = 0; i < dag.getNodesCount(); ++i)
        {
            const auto & node = dag[i];

            if (node.type == CompileDAG::CompileType::FUNCTION)
                nested_functions.emplace_back(node.function);
            else if (node.type == CompileDAG::CompileType::INPUT)
                argument_types.emplace_back(node.result_type);
        }
    }

    void setCompiledFunction(std::shared_ptr<CompiledFunction> compiled_function_)
    {
        compiled_function = compiled_function_;
    }

    bool isCompilable() const override { return true; }

    llvm::Value * compile(llvm::IRBuilderBase & builder, Values values) const override
    {
        return dag.compile(builder, values);
    }

    String getName() const override { return name; }

    const DataTypes & getArgumentTypes() const override { return argument_types; }

    const DataTypePtr & getResultType() const override { return dag.back().result_type; }

    ExecutableFunctionPtr prepare(const ColumnsWithTypeAndName &) const override
    {
        if (!compiled_function)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Compiled function was not initialized {}", name);

        return std::make_unique<LLVMExecutableFunction>(name, compiled_function);
    }

    bool isDeterministic() const override
    {
        for (const auto & f : nested_functions)
            if (!f->isDeterministic())
                return false;

        return true;
    }

    bool isDeterministicInScopeOfQuery() const override
    {
        for (const auto & f : nested_functions)
            if (!f->isDeterministicInScopeOfQuery())
                return false;

        return true;
    }

    bool isSuitableForConstantFolding() const override
    {
        for (const auto & f : nested_functions)
            if (!f->isSuitableForConstantFolding())
                return false;

        return true;
    }

    bool isInjective(const ColumnsWithTypeAndName & sample_block) const override
    {
        for (const auto & f : nested_functions)
            if (!f->isInjective(sample_block))
                return false;

        return true;
    }

    bool hasInformationAboutMonotonicity() const override
    {
        for (const auto & f : nested_functions)
            if (!f->hasInformationAboutMonotonicity())
                return false;

        return true;
    }

    Monotonicity getMonotonicityForRange(const IDataType & type, const Field & left, const Field & right) const override
    {
        const IDataType * type_ptr = &type;
        Field left_mut = left;
        Field right_mut = right;
        Monotonicity result(true, true, true);
        /// monotonicity is only defined for unary functions, so the chain must describe a sequence of nested calls
        for (size_t i = 0; i < nested_functions.size(); ++i)
        {
            Monotonicity m = nested_functions[i]->getMonotonicityForRange(*type_ptr, left_mut, right_mut);
            if (!m.is_monotonic)
                return m;
            result.is_positive ^= !m.is_positive;
            result.is_always_monotonic &= m.is_always_monotonic;
            if (i + 1 < nested_functions.size())
            {
                if (left_mut != Field())
                    applyFunction(*nested_functions[i], left_mut);
                if (right_mut != Field())
                    applyFunction(*nested_functions[i], right_mut);
                if (!m.is_positive)
                    std::swap(left_mut, right_mut);
                type_ptr = nested_functions[i]->getResultType().get();
            }
        }
        return result;
    }

    static void applyFunction(IFunctionBase & function, Field & value)
    {
        const auto & type = function.getArgumentTypes().at(0);
        ColumnsWithTypeAndName args{{type->createColumnConst(1, value), type, "x" }};
        auto col = function.execute(args, function.getResultType(), 1);
        col->get(0, value);
    }

private:
    std::string name;
    CompileDAG dag;
    DataTypes argument_types;
    std::vector<FunctionBasePtr> nested_functions;
    std::shared_ptr<CompiledFunction> compiled_function;
};

static FunctionBasePtr compile(
    const CompileDAG & dag,
    size_t min_count_to_compile_expression)
{
    static std::unordered_map<UInt128, UInt64, UInt128Hash> counter;
    static std::mutex mutex;

    auto hash_key = dag.hash();
    {
        std::lock_guard lock(mutex);
        if (counter[hash_key]++ < min_count_to_compile_expression)
            return nullptr;
    }

    LOG_TRACE(getLogger(), "Try to compile expression {}", dag.dump());

    FunctionBasePtr fn;

    if (auto * compilation_cache = CompiledExpressionCacheFactory::instance().tryGetCache())
    {
        auto llvm_function = std::make_shared<LLVMFunction>(dag);

        auto [compiled_function_cache_entry, was_inserted] = compilation_cache->getOrSet(hash_key, [&] ()
        {
            CHJIT::CompiledModuleInfo compiled_module_info = compileFunction(getJITInstance(), *llvm_function);
            auto * compiled_jit_function = getJITInstance().findCompiledFunction(compiled_module_info, llvm_function->getName());
            auto compiled_function = std::make_shared<CompiledFunction>(compiled_jit_function, compiled_module_info);

            return std::make_shared<CompiledFunctionCacheEntry>(std::move(compiled_function), compiled_module_info.size);
        });

        if (was_inserted)
            LOG_TRACE(getLogger(),
                "Put compiled expression {} in cache used cache size {} total cache size {}",
                llvm_function->getName(),
                compilation_cache->weight(),
                compilation_cache->maxSize());
        else
            LOG_TRACE(getLogger(), "Get compiled expression {} from cache", llvm_function->getName());

        llvm_function->setCompiledFunction(compiled_function_cache_entry->getCompiledFunction());

        fn = llvm_function;
    }
    else
    {
        fn = std::make_shared<LLVMFunction>(dag);
    }

    LOG_TRACE(getLogger(), "Use compiled expression {}", fn->getName());

    return fn;
}

static bool isCompilableConstant(const ActionsDAG::Node & node)
{
    return node.column && isColumnConst(*node.column) && canBeNativeType(*node.result_type);
}

static bool isCompilableFunction(const ActionsDAG::Node & node)
{
    if (node.type != ActionsDAG::ActionType::FUNCTION)
        return false;

    auto & function = *node.function_base;

    if (!canBeNativeType(*function.getResultType()))
        return false;

    for (const auto & type : function.getArgumentTypes())
    {
        if (!canBeNativeType(*type))
            return false;
    }

    return function.isCompilable();
}

static CompileDAG getCompilableDAG(
    const ActionsDAG::Node * root,
    ActionsDAG::NodeRawConstPtrs & children)
{
    /// Extract CompileDAG from root actions dag node.

    CompileDAG dag;

    std::unordered_map<const ActionsDAG::Node *, size_t> visited_node_to_compile_dag_position;

    struct Frame
    {
        const ActionsDAG::Node * node;
        size_t next_child_to_visit = 0;
    };

    std::stack<Frame> stack;
    stack.emplace(Frame{.node = root});

    while (!stack.empty())
    {
        auto & frame = stack.top();
        const auto * node = frame.node;

        bool is_compilable_constant = isCompilableConstant(*node);
        bool is_compilable_function = isCompilableFunction(*node);

        if (!is_compilable_function || is_compilable_constant)
        {
           CompileDAG::Node compile_node;
           compile_node.function = node->function_base;
           compile_node.result_type = node->result_type;

           if (is_compilable_constant)
           {
               compile_node.type = CompileDAG::CompileType::CONSTANT;
               compile_node.column = node->column;
           }
           else
           {
                compile_node.type = CompileDAG::CompileType::INPUT;
                children.emplace_back(node);
           }

           visited_node_to_compile_dag_position[node] = dag.getNodesCount();
           dag.addNode(std::move(compile_node));
           stack.pop();
           continue;
        }

        while (frame.next_child_to_visit < node->children.size())
        {
            const auto & child = node->children[frame.next_child_to_visit];

            if (visited_node_to_compile_dag_position.contains(child))
            {
                ++frame.next_child_to_visit;
                continue;
            }

            stack.emplace(Frame{.node = child});
            break;
        }

        bool all_children_visited = frame.next_child_to_visit == node->children.size();

        if (!all_children_visited)
            continue;

        /// Here we process only functions that are not compiled constants

        CompileDAG::Node compile_node;
        compile_node.function = node->function_base;
        compile_node.result_type = node->result_type;
        compile_node.type = CompileDAG::CompileType::FUNCTION;

        for (const auto * child : node->children)
            compile_node.arguments.push_back(visited_node_to_compile_dag_position[child]);

        visited_node_to_compile_dag_position[node] = dag.getNodesCount();

        dag.addNode(std::move(compile_node));
        stack.pop();
    }

    return dag;
}

void ActionsDAG::compileFunctions(size_t min_count_to_compile_expression)
{
    struct Data
    {
        bool is_compilable_in_isolation = false;
        bool all_parents_compilable = true;
        size_t compilable_children_size = 0;
        size_t children_size = 0;
    };

    std::unordered_map<const Node *, Data> node_to_data;

    /// Check which nodes can be compiled in isolation

    for (const auto & node : nodes)
    {
        bool node_is_compilable_in_isolation = isCompilableFunction(node) && !isCompilableConstant(node);
        node_to_data[&node].is_compilable_in_isolation = node_is_compilable_in_isolation;
    }

    struct Frame
    {
        const Node * node;
        size_t next_child_to_visit = 0;
    };

    std::stack<Frame> stack;
    std::unordered_set<const Node *> visited_nodes;

    /** Algorithm is to iterate over each node in ActionsDAG, and update node compilable_children_size.
      * After this procedure data for each node is initialized.
      */

    for (auto & node : nodes)
    {
        if (visited_nodes.contains(&node))
            continue;

        stack.emplace(Frame{.node = &node});

        while (!stack.empty())
        {
            auto & current_frame = stack.top();
            auto & current_node = current_frame.node;

            while (current_frame.next_child_to_visit < current_node->children.size())
            {
                const auto & child = node.children[current_frame.next_child_to_visit];

                if (visited_nodes.contains(child))
                {
                    ++current_frame.next_child_to_visit;
                    continue;
                }

                stack.emplace(Frame{.node=child});
                break;
            }

            bool all_children_visited = current_frame.next_child_to_visit == current_node->children.size();

            if (!all_children_visited)
                continue;

            auto & current_node_data = node_to_data[current_node];

            if (current_node_data.is_compilable_in_isolation)
            {
                for (const auto * child : current_node->children)
                {
                    auto & child_data = node_to_data[child];

                    if (child_data.is_compilable_in_isolation)
                    {
                        current_node_data.compilable_children_size += child_data.compilable_children_size;
                        current_node_data.compilable_children_size += 1;
                    }

                    current_node_data.children_size += node_to_data[child].children_size;
                }

                current_node_data.children_size += current_node->children.size();
            }

            visited_nodes.insert(current_node);
            stack.pop();
        }
    }

    for (const auto & node : nodes)
    {
        auto & node_data = node_to_data[&node];
        bool node_is_valid_for_compilation = node_data.is_compilable_in_isolation && node_data.compilable_children_size > 0;

        for (const auto & child : node.children)
            node_to_data[child].all_parents_compilable &= node_is_valid_for_compilation;
    }

    for (const auto & node : index)
    {
        /// Force result nodes to compile
        node_to_data[node].all_parents_compilable = false;
    }

    std::vector<Node *> nodes_to_compile;

    for (auto & node : nodes)
    {
        auto & node_data = node_to_data[&node];

        bool node_is_valid_for_compilation = node_data.is_compilable_in_isolation && node_data.compilable_children_size > 0;

        /// If all parents are compilable then this node should not be standalone compiled
        bool should_compile = node_is_valid_for_compilation && !node_data.all_parents_compilable;

        if (!should_compile)
            continue;

        nodes_to_compile.emplace_back(&node);
    }

    /** Sort nodes before compilation using their children size to avoid compiling subexpression before compile parent expression.
      * This is needed to avoid compiling expression more than once with different names because of compilation order.
      */
    std::sort(nodes_to_compile.begin(), nodes_to_compile.end(), [&](const Node * lhs, const Node * rhs) { return node_to_data[lhs].children_size > node_to_data[rhs].children_size; });

    for (auto & node : nodes_to_compile)
    {
        NodeRawConstPtrs new_children;
        auto dag = getCompilableDAG(node, new_children);

        if (dag.getInputNodesCount() == 0)
            continue;

        if (auto fn = compile(dag, min_count_to_compile_expression))
        {
            ColumnsWithTypeAndName arguments;
            arguments.reserve(new_children.size());
            for (const auto * child : new_children)
                arguments.emplace_back(child->column, child->result_type, child->result_name);

            node->type = ActionsDAG::ActionType::FUNCTION;
            node->function_base = fn;
            node->function = fn->prepare(arguments);
            node->children.swap(new_children);
            node->is_function_compiled = true;
            node->column = nullptr;
        }
    }
}

CompiledExpressionCacheFactory & CompiledExpressionCacheFactory::instance()
{
    static CompiledExpressionCacheFactory factory;
    return factory;
}

void CompiledExpressionCacheFactory::init(size_t cache_size)
{
    if (cache)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CompiledExpressionCache was already initialized");

    cache = std::make_unique<CompiledExpressionCache>(cache_size);
}

CompiledExpressionCache * CompiledExpressionCacheFactory::tryGetCache()
{
    return cache.get();
}

}

#endif
