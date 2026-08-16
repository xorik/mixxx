# included from src/rendergraph/CMakeLists.txt

set(
  shaders
  pattern.frag
  pattern.vert
  rgb.frag
  rgb.vert
  rgba.frag
  rgba.vert
  texture.frag
  texture.vert
  unicolor.frag
  unicolor.vert
)

if(TARGET rendergraph_sg)
  qt6_add_shaders(rendergraph_sg "shaders-qsb"
      BATCHABLE
      PRECOMPILE
      # Must match the rendergraph_gl invocation below: both write the same
      # .qsb files, so the two commands have to stay identical or CMake sees
      # two different rules for one output.
      GLSL
          "100 es,120,150,410"
      # FIXME qsl relies on 'spirv-opt' to be available in the path, so only installing it via VCPKG is not sufficient
      # See https://doc.qt.io/qt-6/qtshadertools-build.html#invoking-external-tools
      # OPTIMIZED
      PREFIX
          /shaders/rendergraph
      FILES
          ${shaders}
  )
endif()

if(TARGET rendergraph_gl)
  # USE_QSHADER_FOR_GL is set in src/rendergraph/CMakeLists.txt when Qt >= 6.6
  if(USE_QSHADER_FOR_GL)
    # Add the .qsb shader bundles; rendergraph::MaterialShader will use
    # QShader to extract the GLSL shader from the bundle.
    message(STATUS "Adding qsb shaders to rendergraph_gl")
    qt6_add_shaders(rendergraph_gl "shaders-qsb"
          BATCHABLE
          PRECOMPILE
          # 410 is needed for an OpenGL 4.1 Core context, which is the newest
          # macOS offers. Without it the bundle only holds 100es/120/150 and a
          # Core context cannot be used at all.
          GLSL
              "100 es,120,150,410"
          # FIXME qsl relies on 'spirv-opt' to be available in the path, so only installing it via VCPKG is not sufficient
          # See https://doc.qt.io/qt-6/qtshadertools-build.html#invoking-external-tools
          # OPTIMIZED
          PREFIX
              /shaders/rendergraph
          FILES
              ${shaders}
    )
  else()
    # Use GLSL shaders extracted from the .qsb shader bundles using
    # generate_shaders_gl.pl
    message(STATUS "Adding gl shaders to rendergraph_gl")
    include(generated_shaders_gl.cmake)

    qt_add_resources(rendergraph_gl "shaders-gl"
          PREFIX
              /shaders/rendergraph
          FILES
              ${generated_shaders_gl}
    )
  endif()
endif()
