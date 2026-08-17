# Build/test image for age_gds: PostgreSQL 16 + Apache AGE + age_gds + test/sql runner.
#
# Usage:
#   docker build -t age_gds-test .
#   docker run --rm age_gds-test
FROM postgres:16-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
        postgresql-server-dev-16 flex bison libreadline-dev zlib1g-dev \
        libomp-dev \
    && rm -rf /var/lib/apt/lists/*

# --- Apache AGE (PG16-compatible branch) ---
RUN git clone --branch PG16 --depth 1 https://github.com/apache/age.git /tmp/age \
    && cd /tmp/age && make -j"$(nproc)" && make install \
    && rm -rf /tmp/age

# --- age_gds ---
WORKDIR /src
COPY . /src

# Note: the mangled-symbol prefix for "NetworKit" (9 chars) is _ZN9NetworKit,
# not _ZN10NetworKit as older docs/README snippets say — verified against the
# actual compiled .so.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && test "$(nm build/age_gds.so | grep -c '_ZN9NetworKit')" -gt 0 \
    && cmake --install build

COPY docker/run-tests.sh /usr/local/bin/run-tests.sh
RUN chmod +x /usr/local/bin/run-tests.sh

ENV POSTGRES_PASSWORD=postgres
CMD ["/usr/local/bin/run-tests.sh"]
