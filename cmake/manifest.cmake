# Writes an OpenXR runtime manifest pointing at the built library with an absolute path
function(puppetxr_write_manifest target path)
    set(content "{\n    \"file_format_version\": \"1.0.0\",\n    \"runtime\": {\n        \"name\": \"puppetxr\",\n     \"library_path\": \"$<TARGET_FILE:${target}>\"\n    }\n}\n")
    file(GENERATE OUTPUT ${path} CONTENT "${content}")
endfunction()
