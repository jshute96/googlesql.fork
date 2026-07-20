#!/bin/bash
# SessionStart hook: prepare a GoogleSQL Bazel build in Claude Code on the web.
#
# The web sandbox starts from a fresh container behind a TLS-inspecting egress
# allowlist. This makes a build possible by (1) installing the pinned Bazel,
# (2) trusting the proxy CA in Bazel's server JVM, and (3) pointing the module
# registry at the GitHub mirror of BCR (bcr.bazel.build is blocked). The host
# Go / autotools the committed MODULE.bazel relies on ship in the image.
# See CLAUDE.md ("Building behind a restricted-network sandbox") for details.
set -euo pipefail

# Only run in remote (Claude Code on the web) environments.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

BAZEL_VERSION="$(tr -d '[:space:]' < "${CLAUDE_PROJECT_DIR}/.bazelversion" 2>/dev/null || echo 7.6.1)"

# 1) Install the pinned Bazel if it isn't already present. The GitHub releases
#    host is on the egress allowlist.
if ! command -v bazel >/dev/null 2>&1; then
  curl -fsSL -o /usr/local/bin/bazel \
    "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-x86_64"
  chmod +x /usr/local/bin/bazel
fi

# 2) Ensure the host Go toolchain is on PATH (rules_go uses it via
#    go_sdk.host()), for this session and for the agent's shells.
if [ -d /usr/local/go/bin ] && ! command -v go >/dev/null 2>&1; then
  export PATH="/usr/local/go/bin:${PATH}"
  echo 'export PATH="/usr/local/go/bin:$PATH"' >> "${CLAUDE_ENV_FILE}"
fi

# 3) Write the user bazelrc with the two settings needed behind the
#    TLS-inspecting egress allowlist. (Kept out of the repo since they are
#    environment-specific.)
cat > "${HOME}/.bazelrc" <<'RC'
# Trust the egress proxy's TLS-inspection CA in Bazel's server JVM (avoids
# PKIX path-building errors on HTTPS downloads). The OS-managed Java cacerts
# already contains it.
startup --host_jvm_args=-Djavax.net.ssl.trustStore=/etc/ssl/certs/java/cacerts
startup --host_jvm_args=-Djavax.net.ssl.trustStorePassword=changeit

# bcr.bazel.build is not on the egress allowlist; use the GitHub-hosted mirror
# of the Bazel Central Registry instead.
common --registry=https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/
RC

# 4) Warn (don't fail) if any host build tool the committed MODULE.bazel relies
#    on is missing from the image.
missing=""
for tool in go make cmake ninja pkg-config autoconf automake m4 tzdata; do
  if [ "${tool}" = "tzdata" ]; then
    [ -d /usr/share/zoneinfo ] || missing="${missing} tzdata"
  else
    command -v "${tool}" >/dev/null 2>&1 || missing="${missing} ${tool}"
  fi
done
[ -n "${missing}" ] && echo "WARNING: missing build prerequisites:${missing}" >&2

echo "GoogleSQL session-start hook complete: $(bazel version 2>/dev/null | sed -n 's/^Build label: /bazel /p')"
