# Enables AddressSanitizer + UndefinedBehaviorSanitizer on a target when
# FPS_ENABLE_SANITIZERS is ON. Not supported with MSVC in this project yet
# (MSVC ASan exists but has linker constraints; revisit if needed).
function(fps_enable_sanitizers target)
    if(FPS_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    endif()
endfunction()
