/**
 * @file btree.cpp
 * @brief Implementacion del arbol B (insercion, escritura a disco y busqueda por rango).
 */

#include "btree.hpp"

#include <algorithm>
#include <cstring>

BTree::BTree() {
    BNode root{};
    root.es_interno = 0;
    root.k = 0;
    root.padding = 0;
    store.append(root);
}

int BTree::lowerBoundKey(const BNode& node, int32_t key) {
    int lo = 0, hi = node.k;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (node.llaves_valores[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void BTree::splitNode(const BNode& full, BNode& left, BNode& right, KV& median) {
    const int mid = BTREE_B / 2 - 1; // indice 0-based del par mediano (posicion BTREE_B/2)

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));

    median = full.llaves_valores[mid];

    left.es_interno = full.es_interno;
    left.k = mid; // BTREE_B/2 - 1 pares a la izquierda
    left.padding = 0;
    if (left.k > 0)
        memcpy(left.llaves_valores, full.llaves_valores, (size_t)left.k * sizeof(KV));

    right.es_interno = full.es_interno;
    right.k = full.k - mid - 1; // BTREE_B/2 pares a la derecha
    right.padding = 0;
    if (right.k > 0)
        memcpy(right.llaves_valores, full.llaves_valores + mid + 1,
               (size_t)right.k * sizeof(KV));

    if (full.es_interno) {
        // Hijos: los primeros BTREE_B/2 van a la izquierda, el resto a la derecha.
        memcpy(left.hijos, full.hijos, (size_t)(mid + 1) * sizeof(int32_t));
        memcpy(right.hijos, full.hijos + mid + 1,
               (size_t)(right.k + 1) * sizeof(int32_t));
    }
}

void BTree::insert(KV p) {
    BNode root = store.read(0);

    if (root.k == BTREE_B) {
        // La raiz esta llena: se divide y se crea una nueva raiz.
        BNode left, right;
        KV median;
        splitNode(root, left, right, median);

        int32_t li = store.append(left);
        int32_t ri = store.append(right);

        BNode newRoot{};
        newRoot.es_interno = 1;
        newRoot.k = 1;
        newRoot.llaves_valores[0] = median;
        newRoot.hijos[0] = li;
        newRoot.hijos[1] = ri;
        newRoot.padding = 0;
        store.write(0, newRoot);

        if (p.key <= median.key) insertNonFull(li, std::move(left), p);
        else insertNonFull(ri, std::move(right), p);
        return;
    }

    insertNonFull(0, std::move(root), p);
}

void BTree::insertNonFull(int32_t idx, BNode node, KV p) {
    if (node.es_interno == 0) {
        // Hoja: insertar el par manteniendo el orden por llave.
        int pos = lowerBoundKey(node, p.key);
        if (pos < node.k)
            memmove(&node.llaves_valores[pos + 1], &node.llaves_valores[pos],
                    (size_t)(node.k - pos) * sizeof(KV));
        node.llaves_valores[pos] = p;
        node.k++;
        store.write(idx, node);
        return;
    }

    int pos = lowerBoundKey(node, p.key);
    int32_t childIdx = node.hijos[pos];
    BNode child = store.read(childIdx);

    if (child.k == BTREE_B) {
        // El hijo esta lleno: se divide antes de descender.
        BNode left, right;
        KV median;
        splitNode(child, left, right, median);

        int32_t li = childIdx;
        store.write(li, left);
        int32_t ri = store.append(right);

        const int oldK = node.k;
        // Insertar el par mediano en el padre (posicion pos).
        memmove(&node.llaves_valores[pos + 1], &node.llaves_valores[pos],
                (size_t)(oldK - pos) * sizeof(KV));
        node.llaves_valores[pos] = median;
        // Insertar el nuevo hijo inmediatamente despues del hijo dividido.
        memmove(&node.hijos[pos + 2], &node.hijos[pos + 1],
                (size_t)(oldK - pos) * sizeof(int32_t));
        node.hijos[pos + 1] = ri;
        node.k = oldK + 1;
        store.write(idx, node);

        if (p.key <= median.key) insertNonFull(li, std::move(left), p);
        else insertNonFull(ri, std::move(right), p);
        return;
    }

    insertNonFull(childIdx, std::move(child), p);
}

void BTree::saveTo(const std::string& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("No se pudo crear el archivo " + path);
    out.write(reinterpret_cast<const char*>(store.nodes.data()),
              (std::streamsize)store.nodes.size() * BLOCK_BYTES);
}

namespace {

/**
 * @brief Recorre un nodo en inorden verificando las invariantes del arbol B.
 * @param nodes     Arreglo de nodos del arbol.
 * @param idx       Indice del nodo actual.
 * @param depth     Profundidad actual (la raiz esta en 0).
 * @param isRoot    Indica si el nodo es la raiz.
 * @param leafDepth Profundidad de la primera hoja encontrada (-1 si aun no hay).
 * @param out       Acumula los pares en inorden.
 * @param err       Descripcion del primer error encontrado.
 */
void validateBNode(const std::vector<BNode>& nodes, int32_t idx, int depth, bool isRoot,
                   int& leafDepth, std::vector<KV>& out, std::string& err) {
    if (!err.empty()) return;
    if (idx < 0 || (size_t)idx >= nodes.size()) {
        err = "indice de nodo invalido";
        return;
    }
    const BNode& n = nodes[(size_t)idx];

    if (n.k < 0 || n.k > BTREE_B) { err = "k fuera de rango"; return; }
    if (!isRoot && n.k < BTREE_B / 2 - 1) { err = "nodo con muy pocos elementos"; return; }
    for (int i = 1; i < n.k; i++)
        if (n.llaves_valores[i - 1].key > n.llaves_valores[i].key) {
            err = "llaves desordenadas";
            return;
        }

    if (n.es_interno == 0) {
        if (leafDepth < 0) leafDepth = depth;
        else if (leafDepth != depth) { err = "hojas a distinta profundidad"; return; }
        for (int i = 0; i < n.k; i++) out.push_back(n.llaves_valores[i]);
        return;
    }

    if (n.k < 1) { err = "nodo interno sin llaves"; return; }

    for (int i = 0; i <= n.k; i++) {
        validateBNode(nodes, n.hijos[i], depth + 1, false, leafDepth, out, err);
        if (!err.empty()) return;
        if (i < n.k) out.push_back(n.llaves_valores[i]);
    }
}

} // namespace

std::string BTree::validate(const std::vector<KV>& esperado) const {
    std::vector<KV> out;
    int leafDepth = -1;
    std::string err;
    validateBNode(store.nodes, 0, 0, true, leafDepth, out, err);
    if (!err.empty()) return err;
    if (out.size() != esperado.size())
        return "cantidad de elementos distinta: " + std::to_string(out.size()) + " vs " +
               std::to_string(esperado.size());
    for (size_t i = 0; i < out.size(); i++)
        if (out[i].key != esperado[i].key)
            return "elemento distinto en posicion " + std::to_string(i);
    return "";
}


void BTree::searchRange(FileStore<BNode>& fs, int32_t idx, int32_t lo, int32_t hi,
                        std::vector<KV>& out) {
    BNode node = fs.read(idx);

    // Revisar los pares almacenados en este nodo.
    for (int i = 0; i < node.k; i++) {
        const KV& kv = node.llaves_valores[i];
        if (kv.key >= lo && kv.key <= hi) out.push_back(kv);
    }

    if (node.es_interno == 0) return;

    // Descender solo en los hijos cuyos rangos intersectan [lo, hi].
    for (int i = 0; i <= node.k; i++) {
        int32_t childLo = (i == 0) ? INT32_MIN : node.llaves_valores[i - 1].key;
        int32_t childHi = (i == node.k) ? INT32_MAX : node.llaves_valores[i].key;
        if (childLo < hi && childHi >= lo) {
            searchRange(fs, node.hijos[i], lo, hi, out);
        }
    }
}
