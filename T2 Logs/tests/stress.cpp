/**
 * @file stress.cpp
 * @brief Pruebas de estres adicionales (no forma parte del entregable principal).
 *
 * Ejecuta inserciones en varios ordenes (aleatorio, ascendente, descendente) y con
 * distintos valores de alpha, verificando invariantes y busquedas por rango contra
 * una implementacion de referencia por fuerza bruta.
 */

#include "../src/btree.hpp"
#include "../src/beps.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

std::vector<KV> bruteRange(const std::vector<KV>& sorted, int32_t lo, int32_t hi) {
    auto cmpLo = [](const KV& a, int32_t k) { return a.key < k; };
    auto cmpHi = [](int32_t k, const KV& a) { return k < a.key; };
    auto a = std::lower_bound(sorted.begin(), sorted.end(), lo, cmpLo);
    auto b = std::upper_bound(a, sorted.end(), hi, cmpHi);
    return std::vector<KV>(a, b);
}

bool samePairs(std::vector<KV> a, std::vector<KV> b) {
    auto cmp = [](const KV& x, const KV& y) {
        return x.key != y.key ? x.key < y.key : x.value < y.value;
    };
    std::sort(a.begin(), a.end(), cmp);
    std::sort(b.begin(), b.end(), cmp);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].key != b[i].key || a[i].value != b[i].value) return false;
    return true;
}

std::vector<KV> makeData(size_t n, int order, uint64_t seed) {
    std::vector<KV> v(n);
    for (size_t i = 0; i < n; i++) v[i] = KV{(int32_t)(1000000 + i * 3), (float)(i % 977)};
    std::mt19937_64 rng(seed);
    if (order == 0) std::shuffle(v.begin(), v.end(), rng);
    else if (order == 2) std::reverse(v.begin(), v.end());
    return v;
}

template <class Tree>
bool testTree(const std::string& name, Tree& tree, const std::vector<KV>& data,
              const std::vector<KV>& sorted, const std::string& path) {
    for (const KV& kv : data) tree.insert(kv);

    std::string err = tree.validate(sorted);
    if (!err.empty()) {
        std::printf("[FAIL] %s invariantes: %s\n", name.c_str(), err.c_str());
        return false;
    }

    tree.saveTo(path);
    tree.releaseMemory();

    FileStore<typename Tree::Node> fs;
    if (!fs.open(path)) {
        std::printf("[FAIL] %s: no se pudo abrir %s\n", name.c_str(), path.c_str());
        return false;
    }

    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int32_t> dist(999000, 1000000 + (int32_t)data.size() * 3 + 1000);
    for (int q = 0; q < 400; q++) {
        int32_t lo = dist(rng);
        int32_t hi = lo + (int32_t)(dist(rng) % 300000);
        std::vector<KV> got;
        Tree::searchRange(fs, 0, lo, hi, got);
        if (!samePairs(got, bruteRange(sorted, lo, hi))) {
            std::printf("[FAIL] %s rango [%d,%d]: %zu vs %zu\n", name.c_str(), lo, hi,
                        got.size(), bruteRange(sorted, lo, hi).size());
            fs.close();
            return false;
        }
    }
    fs.close();
    std::printf("[OK]   %s (%zu pares)\n", name.c_str(), data.size());
    return true;
}

} // namespace

int main() {
    bool ok = true;
    const size_t N = 300000;

    for (int order = 0; order < 3; order++) {
        std::vector<KV> data = makeData(N, order, 100 + order);
        std::vector<KV> sorted = data;
        std::sort(sorted.begin(), sorted.end(),
                  [](const KV& a, const KV& b) { return a.key < b.key; });

        const char* oname = order == 0 ? "aleatorio" : (order == 1 ? "ascendente" : "descendente");
        std::printf("--- orden %s ---\n", oname);

        {
            BTree t;
            t.reserve(N / 150 + 1024);
            ok &= testTree(std::string("B/") + oname, t, data, sorted, "stress_b.bin");
        }
        for (double alpha : {0.1, 0.5, 0.9}) {
            EpsTree<EPS_C_025, EPS_L_025> t(alpha, N / 200 + 1024);
            char buf[64];
            std::snprintf(buf, sizeof buf, "Be025(a=%.1f)/%s", alpha, oname);
            ok &= testTree(buf, t, data, sorted, "stress_e1.bin");
        }
        {
            EpsTree<EPS_C_075, EPS_L_075> t(0.5, N / 200 + 1024);
            ok &= testTree(std::string("Be075/") + oname, t, data, sorted, "stress_e2.bin");
        }
    }

    std::printf(ok ? "\nSTRESS OK\n" : "\nSTRESS FALLIDO\n");
    return ok ? 0 : 1;
}
