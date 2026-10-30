#!/usr/bin/env python3
"""
Graficos de la Tarea 2 (arboles B y B-epsilon).

Uso:
    python3 scripts/plots.py resultados

Lee resultados/resultados.csv (y opcionalmente alpha.csv y los CSV de rango)
y genera las figuras en el mismo directorio.
"""

import csv
import os
import sys
from collections import defaultdict
from datetime import datetime, timezone

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


LABELS = {
    "B": "Arbol B (b=340)",
    "Be_c4_l506": "Bε (c=4, lmax=506, ε≈0.25)",
    "Be_c126_l384": "Bε (c=126, lmax=384, ε≈0.75)",
}
COLORS = {"B": "#1f77b4", "Be_c4_l506": "#d62728", "Be_c126_l384": "#2ca02c"}
MARKERS = {"B": "o", "Be_c4_l506": "s", "Be_c126_l384": "^"}


def leer_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def agrupar(rows, campo):
    """Devuelve {estructura: [(N, valor), ...]} ordenado por N."""
    out = defaultdict(list)
    for r in rows:
        out[r["estructura"]].append((int(r["N"]), float(r[campo])))
    for k in out:
        out[k].sort()
    return out


def figura_metrica(rows, campo, ylabel, titulo, path, logy=False):
    data = agrupar(rows, campo)
    fig, ax = plt.subplots(figsize=(8, 5))
    for est, pts in data.items():
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        ax.plot(xs, ys, marker=MARKERS.get(est, "o"), color=COLORS.get(est),
                label=LABELS.get(est, est), linewidth=1.6, markersize=5)
    ax.set_xscale("log", base=2)
    if logy:
        ax.set_yscale("log")
    ax.set_xlabel("N (cantidad de pares insertados)")
    ax.set_ylabel(ylabel)
    ax.set_title(titulo)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print("escrito", path)


def figura_alpha(path_csv, out_dir):
    rows = leer_csv(path_csv)
    rows.sort(key=lambda r: float(r["alpha"]))
    alphas = [float(r["alpha"]) for r in rows]
    build = [float(r["tiempo_creacion_ms"]) for r in rows]
    ios = [float(r["ios_insercion"]) for r in rows]
    search = [float(r["tiempo_busqueda_ms"]) for r in rows]

    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
    for ax, ys, ylabel, titulo in (
        (axes[0], build, "Tiempo de creacion (ms)", "Tiempo de carga"),
        (axes[1], ios, "I/Os de insercion", "I/Os de carga"),
        (axes[2], search, "Tiempo promedio de busqueda (ms)", "Tiempo de busqueda"),
    ):
        ax.plot(alphas, ys, marker="o", color="#9467bd")
        ax.set_xlabel("alpha")
        ax.set_ylabel(ylabel)
        ax.set_title(titulo)
        ax.grid(True, alpha=0.3)
    fig.suptitle("Barrido de alpha (Bε c=4, lmax=506)")
    fig.tight_layout()
    out = os.path.join(out_dir, "fig_alpha.png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("escrito", out)


def figura_rango(out_dir):
    pares = [("rango_B_N2e20.csv", "N = 2^20", "#1f77b4"),
             ("rango_B_N2e26.csv", "N = 2^26", "#d62728")]
    disponibles = [(f, l, c) for f, l, c in pares
                   if os.path.exists(os.path.join(out_dir, f))]
    if len(disponibles) < 2:
        return

    fig, ax = plt.subplots(figsize=(10, 5))
    for fname, label, color in disponibles:
        rows = leer_csv(os.path.join(out_dir, fname))
        ts = [datetime.fromtimestamp(int(r["timestamp"]), tz=timezone.utc) for r in rows]
        temps = [float(r["temperatura"]) for r in rows]
        ax.plot(ts, temps, linewidth=1.2, color=color, label=label)
    ax.set_xlabel("Fecha (UTC)")
    ax.set_ylabel("Temperatura (°C)")
    ax.set_title("Misma ventana de una semana para dos valores de N")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.autofmt_xdate()
    fig.tight_layout()
    out = os.path.join(out_dir, "fig_rango_N20_vs_N26.png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("escrito", out)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "resultados"
    csv_path = os.path.join(out_dir, "resultados.csv")
    if not os.path.exists(csv_path):
        sys.exit("No existe " + csv_path + " (corre primero el modo experiment)")

    rows = leer_csv(csv_path)

    figura_metrica(rows, "tiempo_creacion_ms", "Tiempo (ms)",
                   "Tiempo de creacion del arbol",
                   os.path.join(out_dir, "fig_tiempo_creacion.png"))
    figura_metrica(rows, "ios_insercion", "I/Os (lecturas + escrituras)",
                   "I/Os simuladas durante la insercion",
                   os.path.join(out_dir, "fig_ios_insercion.png"), logy=True)
    figura_metrica(rows, "tamano_bytes", "Tamano en disco (bytes)",
                   "Tamano del arbol",
                   os.path.join(out_dir, "fig_tamano.png"), logy=True)
    figura_metrica(rows, "tiempo_busqueda_ms", "Tiempo promedio (ms)",
                   "Tiempo promedio de busqueda por rango",
                   os.path.join(out_dir, "fig_tiempo_busqueda.png"))
    figura_metrica(rows, "ios_busqueda", "I/Os promedio por busqueda",
                   "I/Os promedio por busqueda por rango",
                   os.path.join(out_dir, "fig_ios_busqueda.png"))

    alpha_csv = os.path.join(out_dir, "alpha.csv")
    if os.path.exists(alpha_csv):
        figura_alpha(alpha_csv, out_dir)

    figura_rango(out_dir)


if __name__ == "__main__":
    main()
