# zedclaw desktop agent - Podman/OCI container
# Multi-stage build: compile with Zig, run with minimal base

# ── Stage 1: Build ──────────────────────────────────────────────────────────
FROM docker.io/library/debian:bookworm-slim AS builder

ARG ZIG_VERSION=0.13.0
ARG TARGETARCH=x86_64

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Download and install Zig
RUN ARCH=$(uname -m) && \
    if [ "$ARCH" = "aarch64" ]; then ZIG_ARCH="aarch64"; else ZIG_ARCH="x86_64"; fi && \
    ZIG_TARBALL="zig-linux-${ZIG_ARCH}-${ZIG_VERSION}.tar.xz" && \
    curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/${ZIG_TARBALL}" -o /tmp/zig.tar.xz && \
    mkdir -p /opt/zig && \
    tar -xf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 && \
    rm /tmp/zig.tar.xz

ENV PATH="/opt/zig:${PATH}"

WORKDIR /build

# Copy source
COPY build.zig .
COPY src/ src/

# Build release binary
RUN zig build -Doptimize=ReleaseSafe && \
    strip zig-out/bin/zedclaw

# ── Stage 2: Runtime ─────────────────────────────────────────────────────────
FROM docker.io/library/debian:bookworm-slim AS runtime

# Install runtime dependencies (CA certs for HTTPS)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && update-ca-certificates

# Create non-root user
RUN useradd -ms /bin/sh zedclaw

# Create data directory
RUN mkdir -p /home/zedclaw/.config/zedclaw && \
    chown -R zedclaw:zedclaw /home/zedclaw/.config

COPY --from=builder /build/zig-out/bin/zedclaw /usr/local/bin/zedclaw

USER zedclaw
WORKDIR /home/zedclaw

# Data directory as volume for persistence
VOLUME ["/home/zedclaw/.config/zedclaw"]

# Default environment
ENV ZEDCLAW_DATA_DIR=/home/zedclaw/.config/zedclaw

# Interactive terminal by default
ENTRYPOINT ["/usr/local/bin/zedclaw"]
