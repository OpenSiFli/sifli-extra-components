from conan import ConanFile


class XiaozhiCoreRecipe(ConanFile):
    name = "xiaozhi_core"
    version = "0.1.0"

    license = "Apache-2.0"
    user = "sifli"
    author = "sifli"
    url = "https://github.com/OpenSiFli/sifli-extra-components"
    homepage = "https://github.com/OpenSiFli/sifli-extra-components"
    description = "Xiaozhi AI voice assistant client: WebSocket/MQTT link, Opus audio path, MCP tools"
    topics = ("xiaozhi", "ai", "voice", "websocket", "opus", "mcp")

    support_sdk_version = "^2.4"

    # The on-board unit test suite lives in this repository but is not part of the
    # published package: it needs an SDK project, a board and sdk.py test to run.
    exports_sources = "*", "!test/*"

    python_requires = "sf-pkg-base/[^1.0]@sifli"
    python_requires_extend = "sf-pkg-base.SourceOnlyBase"

    def requirements(self):
        # Everything the component uses (lwIP, mbedTLS, cJSON, webclient, libopus)
        # ships with the SiFli SDK, so there is no external Conan dependency.
        pass
