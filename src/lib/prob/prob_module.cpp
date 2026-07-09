#include "../jc2_extension_cpp.h"
#include "Probability.h"
#include <memory>
#include <iostream>

static jc2::Class* g_distClass = nullptr;

static std::shared_ptr<jc::Distribution> getDist(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("Type Error: Expected a Distribution instance.");
    auto ptr = val.get_native_data<std::shared_ptr<jc::Distribution>>();
    if (!ptr) jc2::throw_error("Type Error: Expected a Distribution native object.");
    return *ptr;
}

static jc2::Value makeDist(jc::Distribution d) {
    jc2::Instance inst(*g_distClass);
    auto data = new std::shared_ptr<jc::Distribution>(std::make_shared<jc::Distribution>(std::move(d)));
    inst.set_native_data(data, [](void* ptr) {
        delete static_cast<std::shared_ptr<jc::Distribution>*>(ptr);
    });
    return inst;
}

#define METHOD(name) JC2_ValueHandle dist_##name(JC2_VMContext, int, JC2_ValueHandle* argv, void*)
#define GET_SELF auto d = getDist(jc2::Value(argv[0]))

METHOD(pdf) { GET_SELF; return jc2::Value(d->pdf(jc2::Value(argv[1]).as_double())).get_handle(); }
METHOD(pmf) { GET_SELF; return jc2::Value(d->pdf(jc2::Value(argv[1]).as_double())).get_handle(); }
METHOD(cdf) { GET_SELF; return jc2::Value(d->cdf(jc2::Value(argv[1]).as_double())).get_handle(); }
METHOD(quantile) { GET_SELF; return jc2::Value(d->quantile(jc2::Value(argv[1]).as_double())).get_handle(); }
METHOD(mean) { GET_SELF; return jc2::Value(d->distMean()).get_handle(); }
METHOD(var) { GET_SELF; return jc2::Value(d->distVar()).get_handle(); }
METHOD(std_dev) { GET_SELF; return jc2::Value(std::sqrt(d->distVar())).get_handle(); }
METHOD(sample) {
    GET_SELF;
    int n = static_cast<int>(std::round(jc2::Value(argv[1]).as_double()));
    if (n <= 0) jc2::throw_error("Runtime Error: sample() count must be positive.");
    auto data = d->sample(n);
    jc2::RealMatrix mat(1, n);
    for (int i = 0; i < n; ++i) mat.set(0, i, data[i]);
    return mat.get_handle();
}

#define FUNC(name) JC2_ValueHandle global_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)

FUNC(gamma) { (void)argc; return jc2::Value(jc::prob::tgamma(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(lgamma) { (void)argc; return jc2::Value(jc::prob::lngamma(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(betaFn) { (void)argc; return jc2::Value(jc::prob::betafn(jc2::Value(argv[0]).as_double(), jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(erf) { (void)argc; return jc2::Value(jc::prob::erf_impl(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(erfc) { (void)argc; return jc2::Value(jc::prob::erfc_impl(jc2::Value(argv[0]).as_double())).get_handle(); }

FUNC(Normal) {
    double mu = argc >= 1 ? jc2::Value(argv[0]).as_double() : 0;
    double sigma = argc >= 2 ? jc2::Value(argv[1]).as_double() : 1;
    return makeDist(jc::Distribution::normal(mu, sigma)).get_handle();
}
FUNC(TDist) { (void)argc; return makeDist(jc::Distribution::studentT(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(Chi2) { (void)argc; return makeDist(jc::Distribution::chiSquared(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(FDist) { (void)argc; return makeDist(jc::Distribution::fDist(jc2::Value(argv[0]).as_double(), jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(ExpDist) { (void)argc; return makeDist(jc::Distribution::exponential(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(GammaDist) { (void)argc; return makeDist(jc::Distribution::gammaDist(jc2::Value(argv[0]).as_double(), jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(BetaDist) { (void)argc; return makeDist(jc::Distribution::betaDist(jc2::Value(argv[0]).as_double(), jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(Uniform) { (void)argc; return makeDist(jc::Distribution::uniformDist(jc2::Value(argv[0]).as_double(), jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(Binom) { (void)argc; return makeDist(jc::Distribution::binomial(static_cast<int>(std::round(jc2::Value(argv[0]).as_double())), jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(Poisson) { (void)argc; return makeDist(jc::Distribution::poisson(jc2::Value(argv[0]).as_double())).get_handle(); }
FUNC(Geom) { (void)argc; return makeDist(jc::Distribution::geometric(jc2::Value(argv[0]).as_double())).get_handle(); }

FUNC(pdf) { (void)argc; return jc2::Value(getDist(jc2::Value(argv[0]))->pdf(jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(pmf) { (void)argc; return jc2::Value(getDist(jc2::Value(argv[0]))->pdf(jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(cdf) { (void)argc; return jc2::Value(getDist(jc2::Value(argv[0]))->cdf(jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(quantile) { (void)argc; return jc2::Value(getDist(jc2::Value(argv[0]))->quantile(jc2::Value(argv[1]).as_double())).get_handle(); }
FUNC(dmean) { (void)argc; return jc2::Value(getDist(jc2::Value(argv[0]))->distMean()).get_handle(); }
FUNC(dvar) { (void)argc; return jc2::Value(getDist(jc2::Value(argv[0]))->distVar()).get_handle(); }
FUNC(dstd) { (void)argc; return jc2::Value(std::sqrt(getDist(jc2::Value(argv[0]))->distVar())).get_handle(); }
FUNC(sample) {
    (void)argc;
    int n = static_cast<int>(std::round(jc2::Value(argv[1]).as_double()));
    if (n <= 0) jc2::throw_error("Runtime Error: sample() count must be positive.");
    auto data = getDist(jc2::Value(argv[0]))->sample(n);
    jc2::RealMatrix mat(1, n);
    for (int i = 0; i < n; ++i) mat.set(0, i, data[i]);
    return mat.get_handle();
}
FUNC(distInfo) {
    (void)argc;
    auto d = getDist(jc2::Value(argv[0]));
    std::cout << "  Distribution: " << d->toString() << std::endl;
    std::cout << "  Type:         " << (d->isDiscrete() ? "Discrete" : "Continuous") << std::endl;
    try { std::cout << "  Mean:         " << d->distMean() << std::endl; } catch (...) {}
    try { std::cout << "  Variance:     " << d->distVar() << std::endl; } catch (...) {}
    try { std::cout << "  Std Dev:      " << std::sqrt(d->distVar()) << std::endl; } catch (...) {}
    return jc2::Value().get_handle();
}

static std::vector<double> extractDS(const jc2::Value& v, const std::string& f) {
    if (v.is_real_matrix()) {
        jc2::RealMatrix mat(v.get_handle());
        std::vector<double> r(mat.rows() * mat.cols());
        for (int i = 0; i < mat.rows(); ++i) {
            for (int j = 0; j < mat.cols(); ++j) {
                r[i * mat.cols() + j] = mat.get(i, j);
            }
        }
        return r;
    }
    if (v.is_complex_matrix()) {
        jc2::ComplexMatrix mat(v.get_handle());
        std::vector<double> r(mat.rows() * mat.cols());
        for (int i = 0; i < mat.rows(); ++i) {
            for (int j = 0; j < mat.cols(); ++j) {
                if (std::abs(mat.get_imag(i, j)) > 1e-15) jc2::throw_error(f + "() requires real data.");
                r[i * mat.cols() + j] = mat.get_real(i, j);
            }
        }
        return r;
    }
    jc2::throw_error(f + "() requires a matrix/vector.");
    return {};
}

static void printTest(const jc::TestResult& r) {
    std::cout << "  " << r.name << std::endl;
    std::cout << "  statistic = " << r.statistic << ",  df = " << r.df
        << ",  p-value = " << r.pValue << std::endl;
    if (r.pValue < 0.001)      std::cout << "  Significance: *** (p < 0.001)" << std::endl;
    else if (r.pValue < 0.01)  std::cout << "  Significance: **  (p < 0.01)" << std::endl;
    else if (r.pValue < 0.05)  std::cout << "  Significance: *   (p < 0.05)" << std::endl;
    else                       std::cout << "  Significance: n.s. (p >= 0.05)" << std::endl;
}

FUNC(ttest) {
    auto data = extractDS(jc2::Value(argv[0]), "ttest");
    double mu0 = argc >= 2 ? jc2::Value(argv[1]).as_double() : 0.0;
    auto r = jc::ttest1(data, mu0);
    printTest(r);
    jc2::RealMatrix mat(1, 3);
    mat.set(0, 0, r.statistic); mat.set(0, 1, r.df); mat.set(0, 2, r.pValue);
    return mat.get_handle();
}
FUNC(ttest2) {
    (void)argc;
    auto d1 = extractDS(jc2::Value(argv[0]), "ttest2"), d2 = extractDS(jc2::Value(argv[1]), "ttest2");
    auto r = jc::ttest2ind(d1, d2);
    printTest(r);
    jc2::RealMatrix mat(1, 3);
    mat.set(0, 0, r.statistic); mat.set(0, 1, r.df); mat.set(0, 2, r.pValue);
    return mat.get_handle();
}
FUNC(ttestP) {
    (void)argc;
    auto d1 = extractDS(jc2::Value(argv[0]), "ttestP"), d2 = extractDS(jc2::Value(argv[1]), "ttestP");
    auto r = jc::ttestPaired(d1, d2);
    printTest(r);
    jc2::RealMatrix mat(1, 3);
    mat.set(0, 0, r.statistic); mat.set(0, 1, r.df); mat.set(0, 2, r.pValue);
    return mat.get_handle();
}
FUNC(chi2test) {
    (void)argc;
    auto obs = extractDS(jc2::Value(argv[0]), "chi2test"), exp = extractDS(jc2::Value(argv[1]), "chi2test");
    auto r = jc::chi2test(obs, exp);
    printTest(r);
    jc2::RealMatrix mat(1, 3);
    mat.set(0, 0, r.statistic); mat.set(0, 1, r.df); mat.set(0, 2, r.pValue);
    return mat.get_handle();
}

FUNC(mean) {
    (void)argc;
    jc2::Value arg(argv[0]);
    if (arg.is_instance() && arg.get_native_data<std::shared_ptr<jc::Distribution>>()) {
        return jc2::Value(getDist(arg)->distMean()).get_handle();
    }
    jc2::throw_error("Type Error: prob.mean() expects a Distribution.");
    return jc2::Value().get_handle();
}
FUNC(var) {
    (void)argc;
    jc2::Value arg(argv[0]);
    if (arg.is_instance() && arg.get_native_data<std::shared_ptr<jc::Distribution>>()) {
        return jc2::Value(getDist(arg)->distVar()).get_handle();
    }
    jc2::throw_error("Type Error: prob.var() expects a Distribution.");
    return jc2::Value().get_handle();
}
FUNC(std_dev) {
    (void)argc;
    jc2::Value arg(argv[0]);
    if (arg.is_instance() && arg.get_native_data<std::shared_ptr<jc::Distribution>>()) {
        return jc2::Value(std::sqrt(getDist(arg)->distVar())).get_handle();
    }
    jc2::throw_error("Type Error: prob.std() expects a Distribution.");
    return jc2::Value().get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_distClass = new jc2::Class("Distribution");
    mod.register_value("Distribution", *g_distClass);

    g_distClass->bind_method("pdf", dist_pdf, 1, 1, false);
    g_distClass->bind_method("pmf", dist_pmf, 1, 1, false);
    g_distClass->bind_method("cdf", dist_cdf, 1, 1, false);
    g_distClass->bind_method("quantile", dist_quantile, 1, 1, false);
    g_distClass->bind_method("mean", dist_mean, 0, 0, false);
    g_distClass->bind_method("var", dist_var, 0, 0, false);
    g_distClass->bind_method("std", dist_std_dev, 0, 0, false);
    g_distClass->bind_method("sample", dist_sample, 1, 1, false);

    mod.register_function("gamma", global_gamma, 1, 1, false);
    mod.register_function("lgamma", global_lgamma, 1, 1, false);
    mod.register_function("betaFn", global_betaFn, 2, 2, false);
    mod.register_function("erf", global_erf, 1, 1, false);
    mod.register_function("erfc", global_erfc, 1, 1, false);

    mod.register_function("Normal", global_Normal, 0, 2, false);
    mod.register_function("TDist", global_TDist, 1, 1, false);
    mod.register_function("Chi2", global_Chi2, 1, 1, false);
    mod.register_function("FDist", global_FDist, 2, 2, false);
    mod.register_function("ExpDist", global_ExpDist, 1, 1, false);
    mod.register_function("GammaDist", global_GammaDist, 2, 2, false);
    mod.register_function("BetaDist", global_BetaDist, 2, 2, false);
    mod.register_function("Uniform", global_Uniform, 2, 2, false);
    mod.register_function("Binom", global_Binom, 2, 2, false);
    mod.register_function("Poisson", global_Poisson, 1, 1, false);
    mod.register_function("Geom", global_Geom, 1, 1, false);

    mod.register_function("pdf", global_pdf, 2, 2, false);
    mod.register_function("pmf", global_pmf, 2, 2, false);
    mod.register_function("cdf", global_cdf, 2, 2, false);
    mod.register_function("quantile", global_quantile, 2, 2, false);
    mod.register_function("dmean", global_dmean, 1, 1, false);
    mod.register_function("dvar", global_dvar, 1, 1, false);
    mod.register_function("dstd", global_dstd, 1, 1, false);
    mod.register_function("sample", global_sample, 2, 2, false);
    mod.register_function("distInfo", global_distInfo, 1, 1, false);

    mod.register_function("ttest", global_ttest, 1, 2, false);
    mod.register_function("ttest2", global_ttest2, 2, 2, false);
    mod.register_function("ttestP", global_ttestP, 2, 2, false);
    mod.register_function("chi2test", global_chi2test, 2, 2, false);

    mod.register_function("mean", global_mean, 1, 1, false);
    mod.register_function("var", global_var, 1, 1, false);
    mod.register_function("std", global_std_dev, 1, 1, false);

    mod.register_help("prob",
        "═══ Probability Distributions & Hypothesis Tests — Native Module ═══\n\n"
        "  Requires: import \"prob\"\n\n"
        "  Distributions are first-class objects backed by native C++ code.\n"
        "  After importing, Distribution objects are class instances\n"
        "  (type → \"Distribution\").\n\n"
        "  Creating Distributions\n"
        "  ──────────────────────\n"
        "    import prob\n\n"
        "    CONTINUOUS:\n"
        "      prob.Normal()              Standard normal N(0,1)\n"
        "      prob.Normal(mu, sigma)     General normal N(μ, σ²)\n"
        "      prob.TDist(df)             Student's t-distribution\n"
        "      prob.Chi2(df)              Chi-squared\n"
        "      prob.FDist(d1, d2)         F-distribution\n"
        "      prob.ExpDist(lambda)       Exponential\n"
        "      prob.GammaDist(shape,rate) Gamma\n"
        "      prob.BetaDist(a, b)        Beta\n"
        "      prob.Uniform(a, b)         Continuous uniform on [a,b)\n\n"
        "    DISCRETE:\n"
        "      prob.Binom(n, p)           Binomial\n"
        "      prob.Poisson(lambda)       Poisson\n"
        "      prob.Geom(p)               Geometric (# trials until first success)\n\n"
        "  Distribution Methods (Object-Oriented API)\n"
        "  ──────────────────────\n"
        "    D.pdf(x)              Probability density / mass\n"
        "    D.cdf(x)              Cumulative distribution P(X ≤ x)\n"
        "    D.quantile(p)         Inverse CDF: find x such that P(X ≤ x) = p\n"
        "    D.mean()              Distribution mean\n"
        "    D.var()               Distribution variance\n"
        "    D.std()               Distribution std dev\n"
        "    D.sample(n)           Draw n random samples → returns a row array\n\n"
        "    * Global variants like distInfo(D), pdf(D, x), and mean(D) are also \n"
        "      supported for backward compatibility and functional piping.\n\n"
        "  Special Math Functions (available after import prob)\n"
        "  ──────────────────────\n"
        "    prob.gamma(x)              Gamma function Γ(x)\n"
        "    prob.lgamma(x)             Log-gamma ln(Γ(x))\n"
        "    prob.betaFn(a, b)          Beta function B(a,b) = Γ(a)Γ(b)/Γ(a+b)\n"
        "    prob.erf(x)                Error function\n"
        "    prob.erfc(x)               Complementary error function 1-erf(x)\n\n"
        "  Hypothesis Tests (returns @[statistic, df, p-value])\n"
        "  ──────────────────────\n"
        "    prob.ttest(X)              One-sample t-test:  H0: μ = 0\n"
        "    prob.ttest(X, mu0)         One-sample t-test:  H0: μ = μ0\n"
        "    prob.ttest2(X, Y)          Welch two-sample:   H0: μ_X = μ_Y\n"
        "    prob.ttestP(X, Y)          Paired t-test:      H0: mean(X-Y) = 0\n"
        "    prob.chi2test(obs, exp)    Chi-squared goodness-of-fit\n\n"
        "    Significance levels: *** (p<0.001)  ** (p<0.01)  * (p<0.05)  n.s.\n\n"
        "  Example\n"
        "  ──────────────────────\n"
        "    import prob\n"
        "    d = prob.Normal(100, 15)\n"
        "    d.cdf(130)                   → 0.9772  (97.72%)\n"
        "    d.quantile(0.95)             → 124.67\n"
        "    d.mean()                     → 100\n"
        "    s = d.sample(1000)\n"
        "    mean(s)                      ≈ 100"
    );

    mod.register_function_help("prob.Normal", "prob.Normal([mu], [sigma])", "Constructs a Normal distribution N(mu, sigma^2).", "prob.Normal(0, 1)");
    mod.register_function_help("prob.TDist", "prob.TDist(df)", "Constructs a Student's t-distribution.", "prob.TDist(10)");
    mod.register_function_help("prob.Chi2", "prob.Chi2(df)", "Constructs a Chi-squared distribution.", "prob.Chi2(5)");
    mod.register_function_help("prob.FDist", "prob.FDist(d1, d2)", "Constructs an F-distribution.", "prob.FDist(5, 10)");
    mod.register_function_help("prob.ExpDist", "prob.ExpDist(lambda)", "Constructs an Exponential distribution.", "prob.ExpDist(1.5)");
    mod.register_function_help("prob.GammaDist", "prob.GammaDist(shape, rate)", "Constructs a Gamma distribution.", "prob.GammaDist(2, 1)");
    mod.register_function_help("prob.BetaDist", "prob.BetaDist(a, b)", "Constructs a Beta distribution.", "prob.BetaDist(2, 5)");
    mod.register_function_help("prob.Uniform", "prob.Uniform(a, b)", "Constructs a Continuous Uniform distribution on [a, b).", "prob.Uniform(0, 10)");
    mod.register_function_help("prob.Binom", "prob.Binom(n, p)", "Constructs a Binomial distribution.", "prob.Binom(10, 0.5)");
    mod.register_function_help("prob.Poisson", "prob.Poisson(lambda)", "Constructs a Poisson distribution.", "prob.Poisson(3)");
    mod.register_function_help("prob.Geom", "prob.Geom(p)", "Constructs a Geometric distribution.", "prob.Geom(0.5)");
    mod.register_function_help("prob.gamma", "prob.gamma(x)", "Returns the Gamma function evaluated at x.", "prob.gamma(5)");
    mod.register_function_help("prob.lgamma", "prob.lgamma(x)", "Returns the natural logarithm of the absolute value of the Gamma function.", "prob.lgamma(5)");
    mod.register_function_help("prob.betaFn", "prob.betaFn(a, b)", "Returns the Beta function evaluated at a and b.", "prob.betaFn(2, 5)");
    mod.register_function_help("prob.erfc", "prob.erfc(x)", "Returns the complementary error function of x.", "prob.erfc(1)");
    mod.register_function_help("prob.ttest", "prob.ttest(X, [mu0])", "Performs a one-sample t-test.", "prob.ttest(data, 0)");
    mod.register_function_help("prob.ttest2", "prob.ttest2(X, Y)", "Performs a Welch two-sample t-test.", "prob.ttest2(group1, group2)");
    mod.register_function_help("prob.ttestP", "prob.ttestP(X, Y)", "Performs a paired t-test.", "prob.ttestP(pre, post)");
    mod.register_function_help("prob.chi2test", "prob.chi2test(obs, exp)", "Performs a Chi-squared goodness-of-fit test.", "prob.chi2test(observed, expected)");
    mod.register_function_help("prob.distInfo", "prob.distInfo(dist)", "Prints information about a Distribution.", "prob.distInfo(prob.Normal(0, 1))");
    mod.register_function_help("prob.dmean", "prob.dmean(dist)", "Returns the theoretical mean of a Distribution (global function variant).", "prob.dmean(prob.Normal(0, 1))");
    mod.register_function_help("prob.dvar", "prob.dvar(dist)", "Returns the theoretical variance of a Distribution (global function variant).", "prob.dvar(prob.Normal(0, 1))");
    mod.register_function_help("prob.dstd", "prob.dstd(dist)", "Returns the theoretical standard deviation of a Distribution (global function variant).", "prob.dstd(prob.Normal(0, 1))");
    mod.register_function_help("prob.pdf", "dist.pdf(x)", "Returns the Probability Density Function (or PMF) evaluated at x.", "prob.Normal(0, 1).pdf(0)");
    mod.register_function_help("prob.cdf", "dist.cdf(x)", "Returns the Cumulative Distribution Function evaluated at x.", "prob.Normal(0, 1).cdf(1.96)");
    mod.register_function_help("prob.quantile", "dist.quantile(p)", "Returns the inverse CDF (quantile) for probability p (0 < p < 1).", "prob.Normal(0, 1).quantile(0.95)");
    mod.register_function_help("prob.sample", "dist.sample(n)", "Draws n random samples from the distribution and returns them as a row vector.", "prob.Normal(100, 15).sample(10)");

    return 0;
}

JC2_EXTENSION_INIT
