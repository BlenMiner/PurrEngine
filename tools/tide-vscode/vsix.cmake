# Builds the VS Code extension as a .vsix, the file `code --install-extension`
# takes: bundles src/ and the language client with esbuild, adds the grammar
# from tools/tide-syntax, and zips it all the way `vsce package` does.
#
# cmake -DSOURCE=<this folder> -DSYNTAX=<tools/tide-syntax> -DWORK=<scratch dir>
#       -DVERSION=<x.y.z> -DOUT=<file.vsix> -P vsix.cmake
#
# Needs Node's npm, and the network the first time (npm ci).

cmake_minimum_required(VERSION 3.25)
foreach(var SOURCE SYNTAX WORK VERSION OUT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "Pass -D${var}=...")
    endif()
endforeach()

find_program(NPM NAMES npm.cmd npm REQUIRED)

function(run)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Building the VS Code extension failed: ${ARGN}")
    endif()
endfunction()

# npm ci again only when the dependencies change.
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${SOURCE}/package.json" "${SOURCE}/package-lock.json" "${SOURCE}/src" DESTINATION "${WORK}")
file(SHA256 "${SOURCE}/package-lock.json" lock)
set(installed "")
if(EXISTS "${WORK}/node_modules/.lock-sha256")
    file(READ "${WORK}/node_modules/.lock-sha256" installed)
endif()
if(NOT installed STREQUAL lock)
    run("${NPM}" ci --no-audit --no-fund)
    file(WRITE "${WORK}/node_modules/.lock-sha256" "${lock}")
endif()
run("${NPM}" run build)

set(stage "${WORK}/vsix")
file(REMOVE_RECURSE "${stage}")
file(READ "${SOURCE}/package.json" manifest)
string(JSON manifest SET "${manifest}" version "\"${VERSION}\"")
file(WRITE "${stage}/extension/package.json" "${manifest}")
file(COPY "${WORK}/out/extension.js" DESTINATION "${stage}/extension/out")
file(COPY "${SOURCE}/README.md" "${SOURCE}/icon.png" "${SYNTAX}/language-configuration.json" DESTINATION "${stage}/extension")
file(COPY "${SOURCE}/icons/tide.svg" DESTINATION "${stage}/extension/icons")
file(COPY "${SYNTAX}/syntaxes/tide.tmLanguage.json" DESTINATION "${stage}/extension/syntaxes")

string(JSON engine GET "${manifest}" engines vscode)
file(WRITE "${stage}/extension.vsixmanifest" "<?xml version=\"1.0\" encoding=\"utf-8\"?>
<PackageManifest Version=\"2.0.0\" xmlns=\"http://schemas.microsoft.com/developer/vsx-schema/2011\" xmlns:d=\"http://schemas.microsoft.com/developer/vsx-schema-design/2011\">
  <Metadata>
    <Identity Language=\"en-US\" Id=\"tide\" Version=\"${VERSION}\" Publisher=\"tide-engine\" />
    <DisplayName>Tide</DisplayName>
    <Description xml:space=\"preserve\">The Tide language</Description>
    <Categories>Programming Languages,Formatters</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id=\"Microsoft.VisualStudio.Code.Engine\" Value=\"${engine}\" />
      <Property Id=\"Microsoft.VisualStudio.Code.ExtensionKind\" Value=\"workspace\" />
    </Properties>
    <Icon>extension/icon.png</Icon>
  </Metadata>
  <Installation>
    <InstallationTarget Id=\"Microsoft.VisualStudio.Code\" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type=\"Microsoft.VisualStudio.Code.Manifest\" Path=\"extension/package.json\" Addressable=\"true\" />
    <Asset Type=\"Microsoft.VisualStudio.Services.Icons.Default\" Path=\"extension/icon.png\" Addressable=\"true\" />
  </Assets>
</PackageManifest>
")
file(WRITE "${stage}/[Content_Types].xml" "<?xml version=\"1.0\" encoding=\"utf-8\"?>
<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">
  <Default Extension=\".json\" ContentType=\"application/json\" />
  <Default Extension=\".js\" ContentType=\"application/javascript\" />
  <Default Extension=\".md\" ContentType=\"text/markdown\" />
  <Default Extension=\".png\" ContentType=\"image/png\" />
  <Default Extension=\".svg\" ContentType=\"image/svg+xml\" />
  <Default Extension=\".vsixmanifest\" ContentType=\"text/xml\" />
</Types>
")

get_filename_component(out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")
file(REMOVE "${OUT}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}" --format=zip -- "[Content_Types].xml" extension.vsixmanifest extension
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Zipping ${OUT} failed")
endif()
