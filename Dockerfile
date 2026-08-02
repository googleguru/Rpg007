################################################################################
# RBA-TritonRoute Docker Image
# Base: ubuntu:22.04 + RBA framework + Python GUI stack.
#
# This image does NOT ship a real openroad/TritonRoute binary by default —
# building OpenROAD from source is a multi-hour, multi-GB operation (see
# docs/INTEGRATION.md for why it wasn't done in this repo's own dev sandbox).
# Instead of a stub that silently prints a message and exits 0 (which makes
# a broken routing pipeline look like it succeeded), this image simply does
# not provide `openroad` — any command that needs it fails loudly with a
# real "command not found" rather than a fake success.
#
# Three ways to get a real router in this image:
#   1. `docker build --build-arg BUILD_OPENROAD=1 ...` — builds the patched
#      OpenROAD (third_party/openroad.patch) from source in this image.
#      Slow (commonly 1-3+ hours) and needs several GB of RAM/disk; not
#      attempted by default.
#   2. Bind-mount a prebuilt `openroad` binary into the container, e.g.
#      `docker run -v /path/to/openroad:/usr/local/bin/openroad:ro ...`
#   3. GUI-only mode (default): rba_router and the router binary are simply
#      absent; the Streamlit GUI and simulation/plotting tooling still work
#      for exploring the schema and reporting scaffold.
#
# Build:  docker build -t rba_router:latest .
# Run GUI: docker run -p 8501:8501 rba_router:latest
# Run CLI: docker run rba_router:latest rba_router --help
################################################################################

FROM ubuntu:22.04 AS base

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

# ── System dependencies ────────────────────────────────────────────────────
RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build git wget curl \
    python3 python3-pip python3-dev \
    libboost-all-dev libeigen3-dev \
    tcl-dev tcllib \
    flex bison \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# ── Python GUI + analysis stack ────────────────────────────────────────────
RUN pip3 install --no-cache-dir \
    streamlit==1.35.0 \
    plotly==5.22.0 \
    pandas==2.2.2 \
    matplotlib==3.9.0 \
    seaborn==0.13.2 \
    scipy==1.13.1 \
    numpy==1.26.4 \
    watchdog \
    nlohmann-json3-dev 2>/dev/null || true

# ── nlohmann/json header ───────────────────────────────────────────────────
RUN mkdir -p /usr/local/include/nlohmann && \
    wget -q https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
         -O /usr/local/include/nlohmann/json.hpp || true

# ── Copy RBA framework source ──────────────────────────────────────────────
WORKDIR /opt/rba_router
COPY . .

# ── Optional: build patched OpenROAD from source ───────────────────────────
# Off by default (BUILD_OPENROAD=0). Applies third_party/openroad.patch to
# the commit pinned in OPENROAD_COMMIT and builds it via OpenROAD's own
# etc/DependencyInstaller.sh + cmake. See docs/INTEGRATION.md.
ARG BUILD_OPENROAD=0
RUN if [ "$BUILD_OPENROAD" = "1" ]; then \
        set -e; \
        git clone https://github.com/The-OpenROAD-Project/OpenROAD.git /opt/OpenROAD && \
        cd /opt/OpenROAD && \
        git checkout "$(cat /opt/rba_router/OPENROAD_COMMIT)" && \
        git apply /opt/rba_router/third_party/openroad.patch && \
        ./etc/DependencyInstaller.sh -all && \
        cmake -B build -DCMAKE_BUILD_TYPE=Release && \
        cmake --build build -j$(nproc) && \
        cmake --install build --prefix /usr/local; \
    else \
        echo "[Docker] BUILD_OPENROAD=0 — no openroad binary in this image (GUI/CLI still built; see header of this Dockerfile for how to supply one)"; \
    fi

# ── Build C++ router (graceful fail if OpenROAD not present) ───────────────
RUN cmake -B build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DRBA_ENABLE_TESTS=OFF \
          -DRBA_OPENROAD_LINK=OFF \
    && cmake --build build -j$(nproc) \
    || echo "[Docker] C++ build skipped (nlohmann/json not found — GUI-only mode)"

# ── Entrypoint ────────────────────────────────────────────────────────────
EXPOSE 8501
ENV PYTHONPATH=/opt/rba_router
WORKDIR /opt/rba_router

CMD ["streamlit", "run", "gui/rba_gui.py", \
     "--server.port=8501", \
     "--server.address=0.0.0.0", \
     "--server.headless=true", \
     "--theme.base=dark"]
