from conan import ConanFile

class LibzipRecipe(ConanFile):
    name = "libzip"
    version = "1.11.3"

    license = "Apache-2.0"
    user = "sifli"
    author = "sifli"
    url = "<Package recipe repository url here, for issues about the package>"
    homepage = "<Package homepage here>"
    description = "<Description of hello package here>"
    topics = ("<Put some tag here>", "<here>", "<and here>")

    support_sdk_version = "^2.4"

    # Sources are located in the same place as this recipe, copy them to the recipe
    exports_sources = "SConscript", "Kconfig", "README.md", "LICENSE", "AUTHORS", "src/*", "lib/*", "*.c", "*.cc", "*.cpp", "*.h"

    python_requires = "sf-pkg-base/[^1.0]@sifli"
    python_requires_extend = "sf-pkg-base.SourceOnlyBase"

    def requirements(self):
        # add your package dependencies here, for example:
        # self.requires("fmt/8.1.1")
        pass
