#pragma once
/**
 * @file common.hpp
 * @brief Tipos, constantes y utilidades compartidas por el arbol B y el arbol B-epsilon.
 *
 * Contiene:
 *  - La estructura llave-valor (KV) usada por ambos arboles.
 *  - Las estructuras de nodo con tamano exacto de 4096 bytes.
 *  - RamStore: "disco simulado" en RAM que contabiliza lecturas/escrituras.
 *  - FileStore: lectura de bloques desde un archivo binario, contabilizando I/Os.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>

#ifndef _WIN32
#include <sys/types.h> // off_t para fseeko
#endif

/** @brief Tamano exacto de un bloque/nodo en bytes. */
constexpr int BLOCK_BYTES = 4096;

/** @brief Maximo de pares llave-valor que caben en un nodo del arbol B. */
constexpr int BTREE_B = 340;

/** @brief Configuracion B-epsilon con epsilon ~ 0.25 (c = 4, lmax = 506). */
constexpr int EPS_C_025 = 4;
constexpr int EPS_L_025 = 506;

/** @brief Configuracion B-epsilon con epsilon ~ 0.75 (c = 126, lmax = 384). */
constexpr int EPS_C_075 = 126;
constexpr int EPS_L_075 = 384;

/** @brief Par llave-valor: llave int de 4 bytes y valor float de 4 bytes. */
struct KV {
    int32_t key;   ///< Llave (tiempo Unix de la medicion).
    float   value; ///< Valor asociado (temperatura medida).
};
static_assert(sizeof(KV) == 8, "KV debe ocupar exactamente 8 bytes");

/**
 * @brief Nodo del arbol B.
 *
 * Layout (4096 bytes):
 *   es_interno (4) + k (4) + llaves_valores (340*8) + hijos (341*4) + padding (4)
 *   = 4 + 4 + 2720 + 1364 + 4 = 4096.
 */
struct BNode {
    int32_t es_interno;                ///< 1 si el nodo es interno, 0 si es hoja.
    int32_t k;                         ///< Cantidad de pares llave-valor almacenados.
    KV      llaves_valores[BTREE_B];   ///< Pares ordenados ascendentemente por llave.
    int32_t hijos[BTREE_B + 1];        ///< Indices en disco de los k+1 hijos.
    int32_t padding;                   ///< Sin uso, fijado en 0 (solo para completar 4096 B).
};
static_assert(sizeof(BNode) == BLOCK_BYTES, "BNode debe ocupar exactamente 4096 bytes");

/**
 * @brief Nodo del arbol B-epsilon.
 *
 * Layout (4096 bytes): 4 + 4 + 4 + 4*c + 4*(c+1) + 8*lmax = 8*(2 + c + lmax).
 * Con c + lmax = 510 se obtienen exactamente 4096 bytes.
 *
 * @tparam C    Maximo de llaves divisorias de un nodo interno.
 * @tparam LMAX Maximo de pares llave-valor que caben en el buffer de un nodo.
 */
template <int C, int LMAX>
struct EpsNode {
    int32_t es_interno;   ///< 1 si el nodo es interno, 0 si es hoja.
    int32_t k_llaves;     ///< Cantidad de llaves divisorias activas (0 en hojas).
    int32_t k_buffer;     ///< Cantidad de pares actualmente en el buffer.
    int32_t llaves[C];    ///< Llaves divisorias ordenadas ascendentemente.
    int32_t hijos[C + 1]; ///< Indices en disco de los k_llaves+1 hijos.
    KV      buffer[LMAX]; ///< Buffer ordenado por llave (log de inserciones / datos en hojas).
    static_assert(C + LMAX == 510, "Se requiere c + lmax = 510 para bloques de 4096 bytes");
};
static_assert(sizeof(EpsNode<EPS_C_025, EPS_L_025>) == BLOCK_BYTES,
              "EpsNode<4,506> debe ocupar exactamente 4096 bytes");
static_assert(sizeof(EpsNode<EPS_C_075, EPS_L_075>) == BLOCK_BYTES,
              "EpsNode<126,384> debe ocupar exactamente 4096 bytes");

/** @brief Contadores de I/Os simuladas. */
struct IoStats {
    uint64_t reads = 0;  ///< Lecturas de bloque.
    uint64_t writes = 0; ///< Escrituras de bloque.
    void reset() { reads = writes = 0; }
};

/**
 * @brief Disco simulado en RAM: un vector de nodos donde cada acceso cuenta como I/O.
 *
 * El elemento 0 del vector es siempre la raiz del arbol.
 *
 * @tparam Node Tipo de nodo (BNode o EpsNode<C,LMAX>).
 */
template <class Node>
class RamStore {
public:
    std::vector<Node> nodes; ///< Nodos del arbol (indice = posicion en "disco").
    IoStats io;              ///< Contadores de lecturas/escrituras.

    /** @brief Reserva capacidad para evitar reubicaciones durante la construccion. */
    void reserve(size_t n) { nodes.reserve(n); }

    /**
     * @brief Lee el nodo en la posicion i (cuenta 1 lectura).
     * @param i Indice del nodo.
     * @return Copia del nodo leido.
     */
    Node read(int32_t i) {
        io.reads++;
        return nodes[(size_t)i];
    }

    /**
     * @brief Escribe el nodo n en la posicion i (cuenta 1 escritura).
     * @param i Indice destino.
     * @param n Nodo a escribir.
     */
    void write(int32_t i, const Node& n) {
        io.writes++;
        nodes[(size_t)i] = n;
    }

    /**
     * @brief Agrega un nodo al final del arreglo (cuenta 1 escritura).
     * @param n Nodo a agregar.
     * @return Indice asignado al nuevo nodo.
     */
    int32_t append(const Node& n) {
        io.writes++;
        nodes.push_back(n);
        return (int32_t)nodes.size() - 1;
    }
};

/** @brief Posiciona un FILE* en un offset de 64 bits de forma portable. */
inline int seekFile(FILE* f, int64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, (long long)offset, SEEK_SET);
#else
    return fseeko(f, (off_t)offset, SEEK_SET);
#endif
}

/**
 * @brief Lector de bloques desde un archivo binario (para las busquedas).
 *
 * Cada llamada a read() cuenta como una lectura de disco.
 *
 * @tparam Node Tipo de nodo almacenado en el archivo.
 */
template <class Node>
class FileStore {
public:
    IoStats io; ///< Contadores de lecturas.

    ~FileStore() { close(); }

    /**
     * @brief Abre el archivo binario del arbol.
     * @param path Ruta del archivo.
     * @return true si se pudo abrir.
     */
    bool open(const std::string& path) {
        close();
        f_ = fopen(path.c_str(), "rb");
        return f_ != nullptr;
    }

    /** @brief Cierra el archivo si estaba abierto. */
    void close() {
        if (f_) { fclose(f_); f_ = nullptr; }
    }

    /**
     * @brief Lee el bloque i del archivo (cuenta 1 lectura).
     * @param i Indice del bloque.
     * @return Nodo leido (ceros si la lectura falla).
     */
    Node read(int32_t i) {
        io.reads++;
        Node n;
        if (!f_ || seekFile(f_, (int64_t)i * BLOCK_BYTES) != 0 ||
            fread(&n, BLOCK_BYTES, 1, f_) != 1) {
            memset(&n, 0, sizeof(Node));
        }
        return n;
    }

private:
    FILE* f_ = nullptr;
};

/** @brief Reloj monotono usado para medir tiempos. */
using Clock = std::chrono::steady_clock;

/** @brief Devuelve el tiempo actual en milisegundos (double). */
inline double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}
