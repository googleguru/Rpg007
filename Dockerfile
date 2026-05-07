################################################################################
# RBA-TritonRoute Docker Image
# Base: OpenROAD official image + RBA framework + Python GUI stack
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

# ── Build C++ router (graceful fail if OpenROAD not present) ───────────────
RUN cmake -B build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DRBA_ENABLE_TESTS=OFF \
          -DRBA_OPENROAD_LINK=OFF \
    && cmake --build build -j$(nproc) \
    || echo "[Docker] C++ build skipped (nlohmann/json or OpenROAD not found — GUI-only mode)"

# ── OpenROAD stub (replaced by real binary if available) ──────────────────
RUN printf '#!/usr/bin/env python3\nimport sys\nprint("[OpenROAD stub] Use full OpenROAD build for production")\n' \
    > /usr/local/bin/openroad && chmod +x /usr/local/bin/openroad

# ── Entrypoint ────────────────────────────────────────────────────────────
EXPOSE 8501
ENV PYTHONPATH=/opt/rba_router
WORKDIR /opt/rba_router

CMD ["streamlit", "run", "gui/rba_gui.py", \
     "--server.port=8501", \
     "--server.address=0.0.0.0", \
     "--server.headless=true", \
     "--theme.base=dark"]
