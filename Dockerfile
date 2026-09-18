# ---------- build ----------
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc libc6-dev make libglpk-dev libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY vendor ./vendor
RUN make

# ---------- runtime ----------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      libglpk40 libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# non-root; no secrets are baked in — ANTHROPIC_API_KEY is injected at run time
RUN useradd -r -u 10001 gridwise
COPY --from=build /src/gridwise /usr/local/bin/gridwise
USER gridwise
ENV PORT=8080
EXPOSE 8080
CMD ["/usr/local/bin/gridwise"]
