/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GCC plugin: round floating-point multiply-adds the way the GameCube does.
 *
 * Slippi Dolphin emulates the console bit for bit, so an online match only
 * stays in sync if this port computes every float the same way.  The console
 * compiler (MWCC) fuses a*b+c into fmadds/fmsubs/fnmsubs.  Dolphin computes a
 * single-precision fused op as round_single(round_double(a*b +- c)), and the
 * product of two floats is exact in double, so that is
 * (float)((double)a*(double)b + (double)c).  The Vita's VFP has no fused
 * multiply-add at all, so without help GCC rounds the product first.
 *
 * The 3DS port (melee-3ds, slippi-beta-v0.1.0) does this with clang: it
 * compiles with -ffp-contract=on, so clang marks every a*b+c inside one
 * expression as llvm.fmuladd, and tools/fp_contract.py rewrites those calls.
 * This codebase needs GCC (disc structs use scalar_storage_order), and GCC
 * has no "within one expression" contraction mode.  So this pass finds the
 * same places by hand, right after GIMPLE goes into SSA form and before any
 * inlining or optimization:
 *
 *   - A product from the same expression is an anonymous temporary used
 *     once.  A product saved in a named local (MU_P in mp_fp.h, or plain
 *     "t = a*b;") has a variable behind it and is never fused.
 *   - Which operand gets fused, and the signs, follow clang's
 *     tryEmitFMulAdd: for x+y the left product first, then the right; for
 *     x-y the left product as a*b + (-y), else the right one as (-a)*b + x.
 *     A negated first factor is PowerPC fnmsubs, -(a*b - c), like
 *     fp_contract.py, so an exact cancellation gives -0 like the console.
 *   - float: (float)((double)a*(double)b +- (double)c), plain double ops.
 *   - double: mp_fma(a, b, c) from fp_exact.c, correctly rounded.  When
 *     both factors have few enough significant bits for the product to be
 *     exact in double, one double multiply and add give the same answer.
 *   - Functions in the no-contract list (the console fused nothing there)
 *     are left alone.  Functions named mp_* are the explicit console ops in
 *     mp_fp.h and fp_exact.c, so they are skipped too.
 *   - A float converted to unsigned char/short goes through int first.
 *     MWCC uses fctiwz (signed, saturating) and keeps the low bits, so
 *     -102.0 becomes 0x9A.  ARM's unsigned convert would give 0.  Nana copies
 *     Popo's stick this way (see fp_contract.py).
 *
 * Use:  -fplugin=fp_contract_plugin.so
 *       -fplugin-arg-fp_contract_plugin-no-contract=tools/slippi/no_contract.txt
 *       -fplugin-arg-fp_contract_plugin-stats=out.txt   (optional, appends)
 * Compile with -ffp-contract=off and without -funsafe-math-optimizations, or
 * GCC is allowed to reorder the double maths this pass writes. */

#include "gcc-plugin.h"
#include "plugin-version.h"

#include "tree.h"
#include "tree-pass.h"
#include "context.h"
#include "function.h"
#include "basic-block.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-ssa.h"
#include "ssa.h"
#include "stringpool.h"
#include "tree-ssanames.h"
#include "real.h"
#include "fold-const.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

int plugin_is_GPL_compatible;

/* ------------------------------------------------------------ settings */

/* One no-contract entry: which kinds stay unfused in a function. */
struct NoContractKinds {
    bool f32 = false;
    bool f64 = false;
};

/* "name" -> kinds, and "file::name" -> kinds for one file's static helper. */
static std::map<std::string, NoContractKinds> s_no_contract;
static std::string s_stats_path;

struct Stats {
    int f32 = 0;
    int f32_fnmsub = 0;
    int f64_fma = 0;
    int f64_exact = 0;
    int f64_fnmsub = 0;
    int skipped_no_contract = 0;
    int narrow_conversions = 0;
};
static Stats s_stats;

static std::string trim(const std::string& text)
{
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        end--;
    }
    return text.substr(start, end - start);
}

/* Same format as the 3DS tools/slippi/no_contract.txt:
 * "<function> [f32] [f64]", no kind means both, '#' starts a comment. */
static bool load_no_contract(const char* path)
{
    FILE* file = fopen(path, "r");
    char buffer[512];

    if (file == NULL) {
        fprintf(stderr, "fp_contract_plugin: cannot open %s\n", path);
        return false;
    }

    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        std::string line = buffer;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        /* Split into words. */
        std::vector<std::string> words;
        size_t pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
            size_t start = pos;
            while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') pos++;
            if (pos > start) words.push_back(line.substr(start, pos - start));
        }

        NoContractKinds& kinds = s_no_contract[words[0]];
        if (words.size() == 1) {
            kinds.f32 = true;
            kinds.f64 = true;
        }
        for (size_t i = 1; i < words.size(); i++) {
            if (words[i] == "f32") kinds.f32 = true;
            else if (words[i] == "f64") kinds.f64 = true;
            else fprintf(stderr, "fp_contract_plugin: %s: unknown kind '%s'\n", path, words[i].c_str());
        }
    }
    fclose(file);
    return true;
}

/* "melee/gr/groldkongo" for ".../src/melee/gr/groldkongo.c": the 3DS list
 * names a file's statics by object stem, the Vita list by this path. */
static std::string current_module(void)
{
    std::string path = main_input_filename ? main_input_filename : "";
    size_t src = path.rfind("src/");
    if (src != std::string::npos) path = path.substr(src + 4);
    size_t dot = path.rfind('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    return path;
}

static NoContractKinds kinds_for_function(const char* name)
{
    NoContractKinds result;
    std::map<std::string, NoContractKinds>::iterator it;

    it = s_no_contract.find(name);
    if (it != s_no_contract.end()) result = it->second;

    it = s_no_contract.find(current_module() + "::" + name);
    if (it != s_no_contract.end()) {
        if (it->second.f32) result.f32 = true;
        if (it->second.f64) result.f64 = true;
    }
    return result;
}

/* ------------------------------------------------------------ helpers */

static bool is_float_type(tree type)
{
    return TREE_CODE(type) == REAL_TYPE && TYPE_PRECISION(type) == 32;
}

static bool is_double_type(tree type)
{
    return TREE_CODE(type) == REAL_TYPE && TYPE_PRECISION(type) == 64;
}

/* True for a temporary GCC made while lowering one expression.  A named
 * local (including MU_P's mu_p_) has a user variable behind it. */
static bool is_expression_temporary(tree value)
{
    tree var;

    if (TREE_CODE(value) != SSA_NAME) return false;
    var = SSA_NAME_VAR(value);
    if (var == NULL_TREE) return true;
    return DECL_ARTIFICIAL(var) && DECL_IGNORED_P(var);
}

/* If value is a product from this expression, used only here, returns the
 * statement that computes it.  Otherwise NULL. */
static gimple* fusable_product(tree value, tree type)
{
    gimple* def;

    if (!is_expression_temporary(value)) return NULL;
    if (!has_single_use(value)) return NULL;
    def = SSA_NAME_DEF_STMT(value);
    if (def == NULL || !is_gimple_assign(def)) return NULL;
    if (gimple_assign_rhs_code(def) != MULT_EXPR) return NULL;
    if (TYPE_MAIN_VARIANT(TREE_TYPE(gimple_assign_lhs(def))) != TYPE_MAIN_VARIANT(type)) return NULL;
    return def;
}

/* clang looks through one negation of the product: -(a*b) + c. */
static gimple* negated_product(tree value, tree type)
{
    gimple* def;

    if (!is_expression_temporary(value)) return NULL;
    if (!has_single_use(value)) return NULL;
    def = SSA_NAME_DEF_STMT(value);
    if (def == NULL || !is_gimple_assign(def)) return NULL;
    if (gimple_assign_rhs_code(def) != NEGATE_EXPR) return NULL;
    return fusable_product(gimple_assign_rhs1(def), type) ? def : NULL;
}

/* If value is "-x" from this expression, returns x, else NULL_TREE.
 * fp_contract.py treats any fneg'd first factor as fnmsubs. */
static tree strip_negation(tree value)
{
    gimple* def;

    if (!is_expression_temporary(value)) return NULL_TREE;
    def = SSA_NAME_DEF_STMT(value);
    if (def == NULL || !is_gimple_assign(def)) return NULL_TREE;
    if (gimple_assign_rhs_code(def) != NEGATE_EXPR) return NULL_TREE;
    return gimple_assign_rhs1(def);
}

/* Upper bound on the significant bits of a double value, like _width() in
 * fp_contract.py: a float widened to double has 24, a small integer its
 * size, a constant its real mantissa length, anything else 53. */
static int significant_bits(tree value)
{
    if (TREE_CODE(value) == REAL_CST) {
        REAL_VALUE_TYPE r = TREE_REAL_CST(value);
        long words[2];
        unsigned long long bits;
        unsigned long long mantissa;
        int trailing = 0;

        if (real_isinf(&r) || real_isnan(&r)) return 53;
        real_to_target(words, &r, DFmode);
        /* real_to_target gives the two 32-bit halves in target word order. */
        if (FLOAT_WORDS_BIG_ENDIAN) {
            bits = ((unsigned long long) (words[0] & 0xffffffffL) << 32) |
                   (unsigned long long) (words[1] & 0xffffffffL);
        } else {
            bits = ((unsigned long long) (words[1] & 0xffffffffL) << 32) |
                   (unsigned long long) (words[0] & 0xffffffffL);
        }
        if ((bits << 1) == 0) return 0;
        mantissa = (bits & ((1ull << 52) - 1)) | (1ull << 52);
        while ((mantissa & 1) == 0) {
            mantissa >>= 1;
            trailing++;
        }
        return 53 - trailing;
    }

    while (true) {
        gimple* def;
        tree source;
        tree source_type;
        enum tree_code code;

        if (!is_expression_temporary(value)) return 53;
        def = SSA_NAME_DEF_STMT(value);
        if (def == NULL || !is_gimple_assign(def)) return 53;
        code = gimple_assign_rhs_code(def);
        source = gimple_assign_rhs1(def);

        /* Look through a negation, it doesn't change the bit count. */
        if (code == NEGATE_EXPR) {
            value = source;
            continue;
        }
        source_type = TREE_TYPE(source);
        if (CONVERT_EXPR_CODE_P(code) && is_float_type(source_type)) return 24;
        if (code == FLOAT_EXPR && INTEGRAL_TYPE_P(source_type)) {
            int precision = TYPE_PRECISION(source_type);
            if (precision <= 16) return precision;
        }
        return 53;
    }
}

static tree s_fma_decl;

/* extern double mp_fma(double, double, double), declared const so it
 * needs no memory operands. */
static tree get_fma_decl(void)
{
    if (s_fma_decl == NULL_TREE) {
        tree type = build_function_type_list(double_type_node, double_type_node,
                                             double_type_node, double_type_node, NULL_TREE);
        s_fma_decl = build_fn_decl("mp_fma", type);
        TREE_READONLY(s_fma_decl) = 1;
        TREE_NOTHROW(s_fma_decl) = 1;
        DECL_EXTERNAL(s_fma_decl) = 1;
        TREE_PUBLIC(s_fma_decl) = 1;
    }
    return s_fma_decl;
}

/* new_ssa = code(op), inserted before the statement at gsi. */
static tree emit_unary(gimple_stmt_iterator* gsi, enum tree_code code, tree type, tree op)
{
    tree result = make_ssa_name(type);
    gimple* stmt = gimple_build_assign(result, code, op);
    gsi_insert_before(gsi, stmt, GSI_SAME_STMT);
    return result;
}

static tree emit_binary(gimple_stmt_iterator* gsi, enum tree_code code, tree type, tree a, tree b)
{
    tree result = make_ssa_name(type);
    gimple* stmt = gimple_build_assign(result, code, a, b);
    gsi_insert_before(gsi, stmt, GSI_SAME_STMT);
    return result;
}

/* ------------------------------------------------------------ rewrite */

/* One a*b + addend to lower.  negate_product: the console op is fnmsub. */
struct Fusion {
    gimple* add_stmt;
    gimple* mul_stmt;
    gimple* neg_stmt;      /* the "-(a*b)" clang looked through, or NULL */
    tree addend;
    bool negate_addend;    /* x - y with x the product: a*b + (-y) */
    bool negate_product;   /* (-a)*b + addend */
};

/* Fills fusion if stmt is a + or - that clang would have turned into
 * llvm.fmuladd.  Mirrors tryEmitFMulAdd and buildFMulAdd. */
static bool find_fusion(gimple* stmt, Fusion* fusion)
{
    enum tree_code code;
    tree type;
    tree lhs_operand;
    tree rhs_operand;
    bool is_sub;
    gimple* mul;
    gimple* neg;

    if (!is_gimple_assign(stmt)) return false;
    code = gimple_assign_rhs_code(stmt);
    if (code != PLUS_EXPR && code != MINUS_EXPR) return false;
    type = TREE_TYPE(gimple_assign_lhs(stmt));
    if (!is_float_type(type) && !is_double_type(type)) return false;

    is_sub = code == MINUS_EXPR;
    lhs_operand = gimple_assign_rhs1(stmt);
    rhs_operand = gimple_assign_rhs2(stmt);
    fusion->add_stmt = stmt;
    fusion->neg_stmt = NULL;

    /* Left operand first: fmuladd(+-a, b, +-rhs). */
    neg = negated_product(lhs_operand, type);
    mul = neg ? SSA_NAME_DEF_STMT(gimple_assign_rhs1(neg)) : fusable_product(lhs_operand, type);
    if (mul != NULL) {
        fusion->mul_stmt = mul;
        fusion->neg_stmt = neg;
        fusion->addend = rhs_operand;
        fusion->negate_addend = is_sub;
        fusion->negate_product = neg != NULL;
        return true;
    }

    /* Then the right one: fmuladd((is_sub ^ neg) ? -a : a, b, lhs). */
    neg = negated_product(rhs_operand, type);
    mul = neg ? SSA_NAME_DEF_STMT(gimple_assign_rhs1(neg)) : fusable_product(rhs_operand, type);
    if (mul != NULL) {
        fusion->mul_stmt = mul;
        fusion->neg_stmt = neg;
        fusion->addend = lhs_operand;
        fusion->negate_addend = false;
        fusion->negate_product = is_sub != (neg != NULL);
        return true;
    }
    return false;
}

static void remove_statement(gimple* stmt)
{
    gimple_stmt_iterator gsi = gsi_for_stmt(stmt);
    gsi_remove(&gsi, true);
    release_defs(stmt);
}

static void lower_fusion(const Fusion& fusion)
{
    gimple_stmt_iterator gsi = gsi_for_stmt(fusion.add_stmt);
    tree result = gimple_assign_lhs(fusion.add_stmt);
    tree type = TREE_TYPE(result);
    tree a = gimple_assign_rhs1(fusion.mul_stmt);
    tree b = gimple_assign_rhs2(fusion.mul_stmt);
    tree addend = fusion.addend;
    bool negated = fusion.negate_product;
    gimple* final_stmt;

    /* "c - K*x" stays fnmsubs even with a constant factor.  That is what
     * MWCC emits.  clang differs here: it folds the negation into K and
     * fp_contract.py then keeps fmadds, which only changes the sign of an
     * exact zero.  GCC has already moved K second (K*x is x*K by now), so
     * the source order isn't known anyway. */

    /* fp_contract.py: a first factor that is itself "-x" makes the whole
     * op fnmsubs.  (A product negated by clang's own rule already set it.)
     * GCC may have swapped "(-a)*b" into "b*(-a)", so check both. */
    if (!negated) {
        tree stripped = strip_negation(a);
        if (stripped != NULL_TREE) {
            a = stripped;
            negated = true;
        } else {
            stripped = strip_negation(b);
            if (stripped != NULL_TREE) {
                b = stripped;
                negated = true;
            }
        }
    }

    if (is_float_type(type)) {
        tree da = emit_unary(&gsi, NOP_EXPR, double_type_node, a);
        tree db = emit_unary(&gsi, NOP_EXPR, double_type_node, b);
        tree dc = emit_unary(&gsi, NOP_EXPR, double_type_node, addend);
        tree product = emit_binary(&gsi, MULT_EXPR, double_type_node, da, db);
        if (fusion.negate_addend) dc = emit_unary(&gsi, NEGATE_EXPR, double_type_node, dc);

        if (negated) {
            /* fnmsubs: -(a*b - c), rounded to double, then to single. */
            tree sum = emit_binary(&gsi, MINUS_EXPR, double_type_node, product, dc);
            tree rounded = emit_unary(&gsi, NOP_EXPR, type, sum);
            final_stmt = gimple_build_assign(result, NEGATE_EXPR, rounded);
            s_stats.f32_fnmsub++;
        } else {
            tree sum = emit_binary(&gsi, PLUS_EXPR, double_type_node, product, dc);
            final_stmt = gimple_build_assign(result, NOP_EXPR, sum);
        }
        s_stats.f32++;
    } else {
        tree c = addend;
        if (fusion.negate_addend) c = emit_unary(&gsi, NEGATE_EXPR, type, c);

        if (significant_bits(a) + significant_bits(b) <= 53) {
            /* The product is exact in double, so one rounded add is the
             * same as a correctly rounded fma. */
            tree product = emit_binary(&gsi, MULT_EXPR, type, a, b);
            if (negated) {
                tree sum = emit_binary(&gsi, MINUS_EXPR, type, product, c);
                final_stmt = gimple_build_assign(result, NEGATE_EXPR, sum);
            } else {
                final_stmt = gimple_build_assign(result, PLUS_EXPR, product, c);
            }
            s_stats.f64_exact++;
        } else {
            gcall* call;
            if (negated) {
                /* fnmsub: -(fma(a, b, -c)). */
                tree minus_c = emit_unary(&gsi, NEGATE_EXPR, type, c);
                tree fused = make_ssa_name(type);
                call = gimple_build_call(get_fma_decl(), 3, a, b, minus_c);
                gimple_call_set_lhs(call, fused);
                gsi_insert_before(&gsi, call, GSI_SAME_STMT);
                final_stmt = gimple_build_assign(result, NEGATE_EXPR, fused);
            } else {
                call = gimple_build_call(get_fma_decl(), 3, a, b, c);
                gimple_call_set_lhs(call, result);
                final_stmt = call;
            }
            s_stats.f64_fma++;
        }
        if (negated) s_stats.f64_fnmsub++;
    }

    gsi_replace(&gsi, final_stmt, false);

    /* The old product (and the negation clang looked through) is unused now. */
    if (fusion.neg_stmt != NULL) remove_statement(fusion.neg_stmt);
    remove_statement(fusion.mul_stmt);
}

/* float -> unsigned char/short: go through int like fctiwz.
 * Returns true if it changed stmt. */
static bool lower_narrow_conversion(gimple* stmt)
{
    tree result;
    tree result_type;
    tree source;

    if (!is_gimple_assign(stmt)) return false;
    if (gimple_assign_rhs_code(stmt) != FIX_TRUNC_EXPR) return false;
    result = gimple_assign_lhs(stmt);
    result_type = TREE_TYPE(result);
    source = gimple_assign_rhs1(stmt);
    if (!INTEGRAL_TYPE_P(result_type) || !TYPE_UNSIGNED(result_type)) return false;
    if (TYPE_PRECISION(result_type) >= 32) return false;
    if (!is_float_type(TREE_TYPE(source)) && !is_double_type(TREE_TYPE(source))) return false;

    gimple_stmt_iterator gsi = gsi_for_stmt(stmt);
    tree as_int = emit_unary(&gsi, FIX_TRUNC_EXPR, integer_type_node, source);
    gsi_replace(&gsi, gimple_build_assign(result, NOP_EXPR, as_int), false);
    s_stats.narrow_conversions++;
    return true;
}

/* ------------------------------------------------------------ the pass */

static const pass_data fp_contract_pass_data = {
    GIMPLE_PASS,
    "gc_fp_contract",       /* -fdump-tree-gc_fp_contract shows the result */
    OPTGROUP_NONE,
    TV_NONE,
    PROP_ssa | PROP_cfg,
    0,
    0,
    0,
    TODO_update_ssa,
};

class fp_contract_pass : public gimple_opt_pass {
public:
    fp_contract_pass(gcc::context* context) : gimple_opt_pass(fp_contract_pass_data, context) {}

    unsigned int execute(function* fun) override
    {
        const char* name;
        NoContractKinds kinds;
        std::vector<Fusion> fusions;
        std::vector<gimple*> conversions;
        std::set<gimple*> used_products;
        basic_block bb;

        if (DECL_NAME(fun->decl) == NULL_TREE) return 0;
        name = IDENTIFIER_POINTER(DECL_NAME(fun->decl));

        /* The explicit console ops do their own rounding. */
        if (strncmp(name, "mp_", 3) == 0) return 0;
        kinds = kinds_for_function(name);

        /* Collect first, then rewrite, so the walk never sees new code. */
        FOR_EACH_BB_FN(bb, fun) {
            for (gimple_stmt_iterator gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
                gimple* stmt = gsi_stmt(gsi);
                Fusion fusion;

                if (is_gimple_assign(stmt) && gimple_assign_rhs_code(stmt) == FIX_TRUNC_EXPR) {
                    conversions.push_back(stmt);
                    continue;
                }
                if (!find_fusion(stmt, &fusion)) continue;

                /* The product of a fused add can't feed a second one. */
                if (used_products.count(fusion.mul_stmt)) continue;

                tree type = TREE_TYPE(gimple_assign_lhs(stmt));
                if ((is_float_type(type) && kinds.f32) || (is_double_type(type) && kinds.f64)) {
                    s_stats.skipped_no_contract++;
                    continue;
                }
                used_products.insert(fusion.mul_stmt);
                fusions.push_back(fusion);
            }
        }

        for (size_t i = 0; i < fusions.size(); i++) lower_fusion(fusions[i]);
        for (size_t i = 0; i < conversions.size(); i++) lower_narrow_conversion(conversions[i]);
        return 0;
    }

    opt_pass* clone() override { return new fp_contract_pass(m_ctxt); }
};

static void write_stats(void* gcc_data, void* user_data)
{
    FILE* file;

    (void) gcc_data;
    (void) user_data;
    if (s_stats_path.empty()) return;
    file = fopen(s_stats_path.c_str(), "a");
    if (file == NULL) return;
    fprintf(file, "%s f32 %d f32_fnmsub %d f64_fma %d f64_exact %d f64_fnmsub %d "
                  "no_contract %d narrow_conversions %d\n",
            current_module().c_str(), s_stats.f32, s_stats.f32_fnmsub, s_stats.f64_fma,
            s_stats.f64_exact, s_stats.f64_fnmsub, s_stats.skipped_no_contract,
            s_stats.narrow_conversions);
    fclose(file);
}

int plugin_init(struct plugin_name_args* info, struct plugin_gcc_version* version)
{
    struct register_pass_info pass_info;

    if (!plugin_default_version_check(version, &gcc_version)) {
        fprintf(stderr, "fp_contract_plugin: built for GCC %s, loaded by a different GCC\n",
                gcc_version.basever);
        return 1;
    }

    for (int i = 0; i < info->argc; i++) {
        if (strcmp(info->argv[i].key, "no-contract") == 0 && info->argv[i].value) {
            if (!load_no_contract(info->argv[i].value)) return 1;
        } else if (strcmp(info->argv[i].key, "stats") == 0 && info->argv[i].value) {
            s_stats_path = info->argv[i].value;
        } else {
            fprintf(stderr, "fp_contract_plugin: unknown argument '%s'\n", info->argv[i].key);
            return 1;
        }
    }

    /* Right after GIMPLE goes into SSA, before early inlining. */
    pass_info.pass = new fp_contract_pass(g);
    pass_info.reference_pass_name = "ssa";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    register_callback(info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    register_callback(info->base_name, PLUGIN_FINISH, write_stats, NULL);
    return 0;
}
