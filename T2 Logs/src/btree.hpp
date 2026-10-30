#pragma once
/**
 * @file btree.hpp
 * @brief Arbol B de orden b = 340 (nodos de 4096 bytes) con insercion y busqueda por rango.
 *
 * La construccion se realiza sobre un arreglo de nodos en RAM (RamStore) contabilizando
 * lecturas y escrituras simuladas. La busqueda por rango se realiza leyendo bloques
 * desde un archivo binario (FileStore).
 */

#include "common.hpp"

/**
 * @brief Arbol B con un maximo de BTREE_B = 340 pares llave-valor por nodo.
 *
 * Invariante: en todo nodo, BTREE_B/2 - 1 <= k <= BTREE_B (salvo la raiz, que puede
 * tener menos). Los pares dentro de un nodo estan ordenados ascendentemente por llave
 * y el hijo i contiene llaves en (llaves_valores[i-1].key, llaves_valores[i].key].
 */
class BTree {
public:
    using Node = BNode; ///< Tipo de nodo usado por el arbol.

    /**
     * @brief Construye un arbol vacio con una raiz hoja sin elementos.
     * @post El arbol contiene un unico nodo (la raiz) en la posicion 0.
     */
    BTree();

    /**
     * @brief Reserva memoria para `n` nodos (evita reubicaciones del vector).
     * @param n Cantidad estimada de nodos.
     */
    void reserve(size_t n) { store.reserve(n); }

    /**
     * @brief Inserta un par llave-valor en el arbol.
     *
     * Si la raiz esta llena, se divide antes de descender. Luego se desciende
     * dividiendo preventivamente todo hijo lleno que se encuentre en el camino.
     *
     * @param p Par llave-valor a insertar.
     */
    void insert(KV p);

    /**
     * @brief Escribe todos los nodos secuencialmente a un archivo binario.
     * @param path Ruta del archivo de salida.
     */
    void saveTo(const std::string& path) const;

    /** @return Cantidad de nodos usados por el arbol. */
    size_t nodeCount() const { return store.nodes.size(); }
    /** @return Lecturas simuladas realizadas durante la construccion. */
    uint64_t insertReads() const { return store.io.reads; }
    /** @return Escrituras simuladas realizadas durante la construccion. */
    uint64_t insertWrites() const { return store.io.writes; }
    /** @return Referencia constante al arreglo de nodos (para validacion). */
    const std::vector<BNode>& nodes() const { return store.nodes; }

    /** @brief Libera la memoria RAM ocupada por los nodos (tras escribir el arbol a disco). */
    void releaseMemory() {
        store.nodes.clear();
        store.nodes.shrink_to_fit();
    }

    /**
     * @brief Verifica las invariantes del arbol B y que contenga exactamente las llaves dadas.
     * @param esperado Pares esperados, ordenados ascendentemente por llave.
     * @return Cadena vacia si todo esta bien; descripcion del error en caso contrario.
     */
    std::string validate(const std::vector<KV>& esperado) const;

    /**
     * @brief Busca todos los pares con llave en [lo, hi] leyendo desde un archivo.
     *
     * @param fs    Lector de bloques del archivo que contiene el arbol.
     * @param idx   Indice del nodo actual (0 para la raiz).
     * @param lo    Limite inferior (inclusivo) del rango.
     * @param hi    Limite superior (inclusivo) del rango.
     * @param out   Vector donde se acumulan los pares encontrados.
     */
    static void searchRange(FileStore<BNode>& fs, int32_t idx, int32_t lo, int32_t hi,
                            std::vector<KV>& out);

private:
    RamStore<BNode> store; ///< Disco simulado en RAM.

    /**
     * @brief Busca la primera posicion i tal que node.llaves_valores[i].key >= key.
     * @param node Nodo donde buscar.
     * @param key  Llave buscada.
     * @return Posicion en [0, k] (k si todas las llaves son menores).
     */
    static int lowerBoundKey(const BNode& node, int32_t key);

    /**
     * @brief Divide un nodo lleno en dos nodos y extrae el par mediano.
     *
     * El nodo de entrada debe tener exactamente BTREE_B pares. El nodo izquierdo
     * recibe los primeros BTREE_B/2 - 1 pares, el derecho los ultimos BTREE_B/2,
     * y el par mediano se retorna por referencia.
     *
     * @param full   Nodo lleno a dividir.
     * @param left   [salida] Nuevo nodo izquierdo.
     * @param right  [salida] Nuevo nodo derecho.
     * @param median [salida] Par llave-valor mediano que sube al padre.
     */
    static void splitNode(const BNode& full, BNode& left, BNode& right, KV& median);

    /**
     * @brief Inserta un par en el subarbol cuya raiz es idx, que NO esta llena.
     *
     * @param idx  Indice del nodo actual.
     * @param node Contenido del nodo actual (ya leido).
     * @param p    Par a insertar.
     */
    void insertNonFull(int32_t idx, BNode node, KV p);
};
