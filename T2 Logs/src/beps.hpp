#pragma once
/**
 * @file beps.hpp
 * @brief Arbol B-epsilon (Bε) con buffer de inserciones pendientes.
 *
 * Cada nodo ocupa exactamente 4096 bytes:
 *   es_interno (4) + k_llaves (4) + k_buffer (4) + llaves[c] (4c)
 *   + hijos[c+1] (4c+4) + buffer[lmax] (8*lmax) = 8*(2 + c + lmax) = 4096
 * cuando c + lmax = 510.
 *
 * La insercion entra siempre por la raiz (O(1) I/Os amortizado): el par se agrega
 * al buffer de la raiz y, cuando este se llena, se vacia (flush) parcialmente hacia
 * los hijos. La busqueda por rango revisa el buffer de cada nodo visitado y desciende
 * por todos los hijos cuyos rangos de llaves intersectan la consulta.
 */

#include "common.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <vector>

/**
 * @brief Arbol B-epsilon parametrizado por c (llaves divisorias) y lmax (buffer).
 *
 * @tparam C    Maximo de llaves divisorias de un nodo interno.
 * @tparam LMAX Maximo de pares llave-valor por buffer. Debe cumplir C + LMAX = 510.
 */
template <int C, int LMAX>
class EpsTree {
public:
    using Node = EpsNode<C, LMAX>; ///< Tipo de nodo del arbol.

    /**
     * @brief Construye un arbol vacio con una raiz hoja sin elementos.
     * @param alpha        Fraccion del promedio minimo que debe recibir un hijo para vaciarlo.
     * @param reserveNodes Cantidad estimada de nodos (reserva de memoria).
     */
    explicit EpsTree(double alpha = 0.5, size_t reserveNodes = 0) : alpha_(alpha) {
        store_.reserve(reserveNodes);
        Node root{};
        root.es_interno = 0;
        root.k_llaves = 0;
        root.k_buffer = 0;
        store_.append(root);
    }

    /**
     * @brief Inserta un par llave-valor en la raiz del arbol.
     * @param p Par a insertar.
     */
    void insert(KV p) {
        Node root = store_.read(0);

        if (root.es_interno == 0) {
            if (root.k_buffer < LMAX) {
                insertSorted(root.buffer, root.k_buffer, p);
                store_.write(0, root);
                return;
            }
            Split s = splitLeaf(root);
            int32_t li = store_.append(s.left);
            int32_t ri = store_.append(s.right);
            store_.write(0, makeRoot(s.med, li, ri, p));
            return;
        }

        if (root.k_buffer == LMAX) {
            FlushRes fr = flush(0, std::move(root));
            if (fr.split) {
                int32_t li = store_.append(fr.left);
                int32_t ri = store_.append(fr.right);
                store_.write(0, makeRoot(fr.med, li, ri, p));
                return;
            }
            root = std::move(fr.node);
        }

        insertSorted(root.buffer, root.k_buffer, p);
        store_.write(0, root);
    }

    /**
     * @brief Escribe todos los nodos secuencialmente a un archivo binario.
     * @param path Ruta del archivo de salida.
     */
    void saveTo(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("No se pudo crear el archivo " + path);
        out.write(reinterpret_cast<const char*>(store_.nodes.data()),
                  (std::streamsize)store_.nodes.size() * BLOCK_BYTES);
    }

    /** @return Cantidad de nodos usados por el arbol. */
    size_t nodeCount() const { return store_.nodes.size(); }
    /** @return Lecturas simuladas realizadas durante la construccion. */
    uint64_t insertReads() const { return store_.io.reads; }
    /** @return Escrituras simuladas realizadas durante la construccion. */
    uint64_t insertWrites() const { return store_.io.writes; }
    /** @return Valor de alpha configurado. */
    double alpha() const { return alpha_; }
    /** @return Referencia constante al arreglo de nodos (para validacion). */
    const std::vector<Node>& nodes() const { return store_.nodes; }

    /**
     * @brief Busca todos los pares con llave en [lo, hi] leyendo desde un archivo.
     *
     * Se revisa el buffer del nodo actual y se desciende por todos los hijos cuyos
     * rangos de llaves intersectan el rango consultado.
     *
     * @param fs  Lector de bloques del archivo que contiene el arbol.
     * @param idx Indice del nodo actual (0 para la raiz).
     * @param lo  Limite inferior (inclusivo).
     * @param hi  Limite superior (inclusivo).
     * @param out Vector donde se acumulan los pares encontrados.
     */
    static void searchRange(FileStore<Node>& fs, int32_t idx, int32_t lo, int32_t hi,
                            std::vector<KV>& out) {
        Node n = fs.read(idx);

        for (int i = 0; i < n.k_buffer; i++) {
            const KV& kv = n.buffer[i];
            if (kv.key >= lo && kv.key <= hi) out.push_back(kv);
        }

        if (n.es_interno == 0) return;

        for (int i = 0; i <= n.k_llaves; i++) {
            int32_t childLo = (i == 0) ? INT32_MIN : n.llaves[i - 1];
            int32_t childHi = (i == n.k_llaves) ? INT32_MAX : n.llaves[i];
            if (childLo < hi && childHi >= lo) {
                searchRange(fs, n.hijos[i], lo, hi, out);
            }
        }
    }

    /** @brief Libera la memoria RAM ocupada por los nodos (tras escribir el arbol a disco). */
    void releaseMemory() {
        store_.nodes.clear();
        store_.nodes.shrink_to_fit();
    }

    /**
     * @brief Verifica las invariantes del arbol y que contenga exactamente los pares dados.
     * @param esperado Pares esperados, ordenados ascendentemente por llave.
     * @return Cadena vacia si todo esta bien; descripcion del error en caso contrario.
     */
    std::string validate(const std::vector<KV>& esperado) const {
        std::vector<KV> encontrado;
        std::string err;
        validateNode(0, INT64_MIN, INT64_MAX, encontrado, err);
        if (!err.empty()) return err;
        if (encontrado.size() != esperado.size())
            return "cantidad de elementos distinta: " + std::to_string(encontrado.size()) +
                   " vs " + std::to_string(esperado.size());
        std::sort(encontrado.begin(), encontrado.end(),
                  [](const KV& a, const KV& b) { return a.key < b.key; });
        for (size_t i = 0; i < encontrado.size(); i++) {
            if (encontrado[i].key != esperado[i].key)
                return "elemento distinto en posicion " + std::to_string(i);
        }
        return "";
    }

private:
    /** @brief Resultado de una division de nodo. */
    struct Split {
        bool    did = false; ///< Indica si hubo division.
        Node    left{};      ///< Nodo izquierdo resultante.
        Node    right{};     ///< Nodo derecho resultante.
        int32_t med = 0;     ///< Llave divisoria que sube al padre.
    };

    /** @brief Resultado de un vaciado de buffer (puede incluir una division del nodo). */
    struct FlushRes {
        bool    split = false; ///< Indica si el nodo se dividio.
        Node    left{};        ///< Nodo izquierdo (si hubo division).
        Node    right{};       ///< Nodo derecho (si hubo division).
        int32_t med = 0;       ///< Llave mediana (si hubo division).
        Node    node{};        ///< Nodo actualizado (si no hubo division).
    };

    RamStore<Node> store_; ///< Disco simulado en RAM.
    double alpha_;         ///< Parametro alpha del vaciado de buffers.

    /** @brief Crea una raiz interna con una llave divisoria y un par en el buffer. */
    static Node makeRoot(int32_t med, int32_t li, int32_t ri, KV p) {
        Node r{};
        r.es_interno = 1;
        r.k_llaves = 1;
        r.k_buffer = 1;
        r.llaves[0] = med;
        r.hijos[0] = li;
        r.hijos[1] = ri;
        r.buffer[0] = p;
        return r;
    }

    /** @brief Inserta un par ordenadamente en un arreglo de tamano fijo. */
    static void insertSorted(KV* buf, int& n, KV p) {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (buf[mid].key < p.key) lo = mid + 1;
            else hi = mid;
        }
        if (lo < n) memmove(buf + lo + 1, buf + lo, (size_t)(n - lo) * sizeof(KV));
        buf[lo] = p;
        n++;
    }

    /** @brief Primera posicion con llave > key (upper_bound). */
    static int upperBoundKey(const KV* buf, int n, int32_t key) {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (buf[mid].key <= key) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    /** @brief Mezcla `items` (ordenados) dentro del buffer del nodo u (ordenado). */
    static void mergeSorted(Node& u, const std::vector<KV>& items) {
        assert((int)items.size() + u.k_buffer <= LMAX);
        KV tmp[LMAX];
        int i = 0, j = 0, o = 0;
        const int m = (int)items.size();
        while (i < u.k_buffer && j < m) {
            if (u.buffer[i].key <= items[j].key) tmp[o++] = u.buffer[i++];
            else tmp[o++] = items[j++];
        }
        while (i < u.k_buffer) tmp[o++] = u.buffer[i++];
        while (j < m) tmp[o++] = items[j++];
        memcpy(u.buffer, tmp, (size_t)o * sizeof(KV));
        u.k_buffer = o;
    }

    /** @brief Indice del hijo childIdx dentro de n, o -1 si no esta. */
    static int indexOfChild(const Node& n, int32_t childIdx) {
        for (int i = 0; i <= n.k_llaves; i++)
            if (n.hijos[i] == childIdx) return i;
        return -1;
    }

    /** @brief Inserta la llave `med` en la posicion pos y el hijo `rightIdx` en pos+1. */
    static void insertKeyChild(Node& v, int pos, int32_t med, int32_t rightIdx) {
        assert(v.k_llaves < C);
        if (pos < v.k_llaves)
            memmove(&v.llaves[pos + 1], &v.llaves[pos],
                    (size_t)(v.k_llaves - pos) * sizeof(int32_t));
        v.llaves[pos] = med;
        memmove(&v.hijos[pos + 2], &v.hijos[pos + 1],
                (size_t)(v.k_llaves - pos) * sizeof(int32_t));
        v.hijos[pos + 1] = rightIdx;
        v.k_llaves++;
    }

    /** @brief Inserta la llave `med` y el hijo `rightIdx` al inicio del nodo v. */
    static void insertKeyChildFront(Node& v, int32_t med, int32_t rightIdx) {
        assert(v.k_llaves < C);
        memmove(&v.llaves[1], &v.llaves[0], (size_t)v.k_llaves * sizeof(int32_t));
        v.llaves[0] = med;
        memmove(&v.hijos[1], &v.hijos[0], (size_t)(v.k_llaves + 1) * sizeof(int32_t));
        v.hijos[0] = rightIdx;
        v.k_llaves++;
    }

    /**
     * @brief Divide una hoja en dos mitades (usada cuando la raiz hoja esta llena).
     * La llave mediana se conserva en la hoja izquierda y sube como copia al padre.
     */
    static Split splitLeaf(const Node& h) {
        Split s;
        s.did = true;
        int half = h.k_buffer / 2;
        if (half < 1) half = 1;
        s.left = h;
        s.right = h;
        s.left.k_buffer = half;
        s.right.k_buffer = h.k_buffer - half;
        if (s.right.k_buffer > 0)
            memcpy(s.right.buffer, h.buffer + half, (size_t)s.right.k_buffer * sizeof(KV));
        s.left.k_llaves = 0;
        s.right.k_llaves = 0;
        s.left.es_interno = 0;
        s.right.es_interno = 0;
        s.med = h.buffer[half - 1].key;
        return s;
    }

    /**
     * @brief Divide una hoja incorporando un lote de pares nuevos.
     *
     * El total (k_buffer + items) es a lo mas 2*lmax, por lo que basta con mezclar
     * todo y partir el arreglo en dos hojas de tamano <= lmax. La llave divisoria
     * es la ultima llave de la hoja izquierda (se conserva alli, como en un B+ tree).
     */
    static Split splitLeafBulk(const Node& h, const std::vector<KV>& items) {
        KV all[2 * LMAX];
        int i = 0, j = 0, o = 0;
        const int m = (int)items.size();
        while (i < h.k_buffer && j < m) {
            if (h.buffer[i].key <= items[j].key) all[o++] = h.buffer[i++];
            else all[o++] = items[j++];
        }
        while (i < h.k_buffer) all[o++] = h.buffer[i++];
        while (j < m) all[o++] = items[j++];

        int half = o / 2;
        if (half < 1) half = 1;

        Split s;
        s.did = true;
        s.left = h;
        s.right = h;
        s.left.es_interno = 0;
        s.right.es_interno = 0;
        s.left.k_llaves = 0;
        s.right.k_llaves = 0;
        s.left.k_buffer = half;
        s.right.k_buffer = o - half;
        memcpy(s.left.buffer, all, (size_t)half * sizeof(KV));
        memcpy(s.right.buffer, all + half, (size_t)(o - half) * sizeof(KV));
        s.med = s.left.buffer[half - 1].key;
        return s;
    }

    /**
     * @brief Divide un nodo interno en la posicion mid (0-based).
     * La llave mid sube al padre y el buffer se reparte segun si la llave es <= o > med.
     */
    static Split splitInternalAt(const Node& v, int mid) {
        Split s;
        s.did = true;
        s.med = v.llaves[mid];

        Node l{}, r{};
        l.es_interno = 1;
        r.es_interno = 1;
        l.k_llaves = mid;
        r.k_llaves = v.k_llaves - mid - 1;

        for (int i = 0; i < l.k_llaves; i++) l.llaves[i] = v.llaves[i];
        for (int i = 0; i < r.k_llaves; i++) r.llaves[i] = v.llaves[mid + 1 + i];
        for (int i = 0; i <= l.k_llaves; i++) l.hijos[i] = v.hijos[i];
        for (int i = 0; i <= r.k_llaves; i++) r.hijos[i] = v.hijos[mid + 1 + i];

        int i = 0;
        while (i < v.k_buffer && v.buffer[i].key <= s.med)
            l.buffer[l.k_buffer++] = v.buffer[i++];
        while (i < v.k_buffer)
            r.buffer[r.k_buffer++] = v.buffer[i++];

        s.left = l;
        s.right = r;
        return s;
    }

    /**
     * @brief Inserta la llave `med` y el hijo `rightIdx` en la mitad correcta de s.
     * @param s        Division de v (izquierda y derecha ya separadas).
     * @param childIdx Hijo de v que se dividio (queda en una de las dos mitades).
     */
    static void placeKeyInHalf(Split& s, int32_t childIdx, int32_t med, int32_t rightIdx) {
        int pl = indexOfChild(s.left, childIdx);
        if (pl >= 0) {
            if (med <= s.med) insertKeyChild(s.left, pl, med, rightIdx);
            else insertKeyChildFront(s.right, med, rightIdx);
        } else {
            int pr = indexOfChild(s.right, childIdx);
            assert(pr >= 0);
            insertKeyChild(s.right, pr, med, rightIdx);
        }
    }

    /** @brief Reparte `items` entre las dos mitades de s y los mezcla en sus buffers. */
    static void mergeIntoHalf(Split& s, const std::vector<KV>& items) {
        if (items.empty()) return;
        std::vector<KV> li, ri;
        li.reserve(items.size());
        ri.reserve(items.size());
        for (const KV& kv : items) {
            if (kv.key <= s.med) li.push_back(kv);
            else ri.push_back(kv);
        }
        if (!li.empty()) mergeSorted(s.left, li);
        if (!ri.empty()) mergeSorted(s.right, ri);
    }

    /**
     * @brief Absorbe la division de un hijo en v.
     *
     * Si v no esta lleno, solo se agrega la llave y el nuevo hijo. Si v esta lleno
     * (k_llaves == C), se divide v en dos nodos y se retorna esa division para que
     * el padre la absorba.
     *
     * @return true si v se dividio (out contiene left, right y med).
     */
    bool absorb(Node& v, int pos, int32_t med, int32_t rightIdx, Split& out) {
        if (v.k_llaves < C) {
            insertKeyChild(v, pos, med, rightIdx);
            return false;
        }
        assert(v.k_llaves == C);
        const int32_t childIdx = v.hijos[pos];
        Split s = splitInternalAt(v, C / 2 - 1);
        placeKeyInHalf(s, childIdx, med, rightIdx);
        out = s;
        return true;
    }

    /**
     * @brief Vacia (parcialmente) el buffer de un nodo interno.
     *
     * Agrupa los pares del buffer por hijo, calcula f = ceil(k_buffer/(k_llaves+1))
     * y vacia, de derecha a izquierda, los hijos que reciben al menos ceil(f*alpha)
     * elementos. Si un hijo se divide, la division se absorbe en este nodo. Si este
     * nodo supera su capacidad de llaves, se divide y se retorna la division al padre.
     *
     * @param vIdx Indice del nodo a vaciar.
     * @param v    Contenido ya leido del nodo.
     * @return Nodo actualizado, o la division producida.
     */
    FlushRes flush(int32_t vIdx, Node v) {
        FlushRes res;
        if (v.k_buffer == 0) {
            res.node = std::move(v);
            return res;
        }

        const int k0 = v.k_llaves;
        const int f = (v.k_buffer + k0) / (k0 + 1); // ceil(k_buffer / (k0+1))
        int thr = (int)std::ceil((double)f * alpha_);
        if (thr < 1) thr = 1;

        Split out;
        std::vector<KV> items;
        items.reserve(LMAX);
        for (int pos = v.k_llaves; pos >= 0; --pos) {
            // Grupo del hijo pos: llaves en (llaves[pos-1], llaves[pos]].
            const int s0 = (pos == 0) ? 0 : upperBoundKey(v.buffer, v.k_buffer, v.llaves[pos - 1]);
            const int s1 = (pos == v.k_llaves) ? v.k_buffer
                                               : upperBoundKey(v.buffer, v.k_buffer, v.llaves[pos]);
            const int m = s1 - s0;
            if (m < thr) continue;

            items.assign(v.buffer + s0, v.buffer + s1);
            if (s1 < v.k_buffer)
                memmove(v.buffer + s0, v.buffer + s1,
                        (size_t)(v.k_buffer - s1) * sizeof(KV));
            v.k_buffer -= m;

            if (pushBatch(v, pos, items, out)) {
                res.split = true;
                res.left = std::move(out.left);
                res.right = std::move(out.right);
                res.med = out.med;
                return res;
            }
        }

        store_.write(vIdx, v);
        res.node = std::move(v);
        return res;
    }

    /**
     * @brief Transfiere un lote ordenado de pares al hijo pos del nodo v.
     *
     * Si el hijo no tiene espacio en su buffer:
     *  - si es hoja, se divide incorporando el lote (a lo mas 2*lmax pares en total);
     *  - si es interno, se vacia su buffer (recursivamente) hasta que haya espacio.
     * Las divisiones del hijo se absorben en v. Si v se llena, se divide y se retorna
     * la division en `out`; los pares pendientes se conservan en los buffers de las
     * dos mitades, por lo que nunca se pierden.
     *
     * @return true si v se dividio (out contiene left, right y med).
     */
    bool pushBatch(Node& v, int pos, std::vector<KV>& items, Split& out) {
        if (items.empty()) return false;

        const int32_t idx = v.hijos[pos];
        Node u = store_.read(idx);

        for (;;) {
            if ((int)items.size() + u.k_buffer <= LMAX) {
                mergeSorted(u, items);
                store_.write(idx, u);
                return false;
            }

            if (u.es_interno == 0) {
                Split s = splitLeafBulk(u, items);
                store_.write(idx, s.left);
                int32_t rIdx = store_.append(s.right);
                return absorb(v, pos, s.med, rIdx, out);
            }

            FlushRes fr = flush(idx, std::move(u));
            if (fr.split) {
                store_.write(idx, fr.left);
                int32_t rIdx = store_.append(fr.right);
                const int32_t childIdx = v.hijos[pos];

                // Si v esta lleno, se guardan los pares en su buffer y se divide v.
                if (v.k_llaves >= C) {
                    assert(v.k_llaves == C);
                    mergeSorted(v, items);
                    Split s = splitInternalAt(v, C / 2 - 1);
                    placeKeyInHalf(s, childIdx, fr.med, rIdx);
                    out = s;
                    return true;
                }

                insertKeyChild(v, pos, fr.med, rIdx);

                std::vector<KV> li, ri;
                li.reserve(items.size());
                ri.reserve(items.size());
                for (const KV& kv : items) {
                    if (kv.key <= fr.med) li.push_back(kv);
                    else ri.push_back(kv);
                }

                if (!li.empty() && pushBatch(v, pos, li, out)) {
                    mergeIntoHalf(out, ri);
                    return true;
                }
                if (!ri.empty() && pushBatch(v, pos + 1, ri, out)) return true;
                return false;
            }

            u = std::move(fr.node);
        }
    }

    /**
     * @brief Verifica recursivamente las invariantes de un nodo y su subarbol.
     * @param idx   Indice del nodo.
     * @param lo    Cota inferior exclusiva del rango de llaves del subarbol.
     * @param hi    Cota superior inclusiva del rango de llaves del subarbol.
     * @param out   Acumula todos los pares encontrados.
     * @param err   Descripcion del primer error encontrado.
     */
    void validateNode(int32_t idx, int64_t lo, int64_t hi, std::vector<KV>& out,
                      std::string& err) const {
        if (!err.empty()) return;
        if (idx < 0 || (size_t)idx >= store_.nodes.size()) {
            err = "indice de nodo invalido";
            return;
        }
        const Node& n = store_.nodes[(size_t)idx];
        if (n.k_llaves < 0 || n.k_llaves > C) { err = "k_llaves fuera de rango"; return; }
        if (n.k_buffer < 0 || n.k_buffer > LMAX) { err = "k_buffer fuera de rango"; return; }

        for (int i = 1; i < n.k_buffer; i++)
            if (n.buffer[i - 1].key > n.buffer[i].key) { err = "buffer desordenado"; return; }
        for (int i = 0; i < n.k_buffer; i++) {
            if ((int64_t)n.buffer[i].key <= lo || (int64_t)n.buffer[i].key > hi) {
                err = "llave de buffer fuera de rango";
                return;
            }
            out.push_back(n.buffer[i]);
        }

        if (n.es_interno == 0) {
            if (n.k_llaves != 0) err = "hoja con llaves divisorias";
            return;
        }

        for (int i = 1; i < n.k_llaves; i++)
            if (n.llaves[i - 1] > n.llaves[i]) { err = "llaves divisorias desordenadas"; return; }

        for (int i = 0; i <= n.k_llaves; i++) {
            int64_t clo = (i == 0) ? lo : std::max(lo, (int64_t)n.llaves[i - 1]);
            int64_t chi = (i == n.k_llaves) ? hi : std::min(hi, (int64_t)n.llaves[i]);
            validateNode(n.hijos[i], clo, chi, out, err);
            if (!err.empty()) return;
        }
    }
};
