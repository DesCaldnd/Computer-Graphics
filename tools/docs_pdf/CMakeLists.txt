# Documentation PDF (tools/docs_pdf/build_docs_pdf.py). No C++ here: a `docs_pdf` target and a pytest ctest test.
# Needs Python with `markdown` (build) and `pytest` + `pypdf` (tests); without them everything is skipped.
# Use a specific interpreter (e.g. a venv) with -DOX_DOCS_PYTHON=path/to/python.
set(OX_DOCS_PYTHON "" CACHE FILEPATH "Python interpreter for tools/docs_pdf (default: find_package(Python3))")
if(OX_DOCS_PYTHON)
    set(_ox_docs_py ${OX_DOCS_PYTHON})
else()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        set(_ox_docs_py ${Python3_EXECUTABLE})
    endif()
endif()
if(NOT _ox_docs_py)
    message(STATUS "docs_pdf: Python not found, PDF target and tests skipped")
    return()
endif()

execute_process(COMMAND ${_ox_docs_py} -c "import markdown, pygments"
                RESULT_VARIABLE _ox_docs_md_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _ox_docs_md_rc EQUAL 0)
    message(STATUS "docs_pdf: python 'markdown'/'pygments' missing (pip install -r tools/docs_pdf/requirements.txt), skipped")
    return()
endif()

add_custom_target(docs_pdf
    COMMAND ${_ox_docs_py} ${CMAKE_CURRENT_SOURCE_DIR}/build_docs_pdf.py
            --root ${PROJECT_SOURCE_DIR} --output ${CMAKE_BINARY_DIR}/docs/OxwaldEngine-docs.pdf
    COMMENT "Building documentation PDF"
    VERBATIM)
set_target_properties(docs_pdf PROPERTIES FOLDER "tools")

if(OX_BUILD_TESTS)
    execute_process(COMMAND ${_ox_docs_py} -c "import pytest, pypdf"
                    RESULT_VARIABLE _ox_docs_pt_rc OUTPUT_QUIET ERROR_QUIET)
    if(_ox_docs_pt_rc EQUAL 0)
        add_test(NAME docs_pdf.pytest
                 COMMAND ${_ox_docs_py} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/tests -q)
        set_tests_properties(docs_pdf.pytest PROPERTIES LABELS "tools" TIMEOUT 300)
    else()
        message(STATUS "docs_pdf: pytest/pypdf missing, tests skipped")
    endif()
endif()
