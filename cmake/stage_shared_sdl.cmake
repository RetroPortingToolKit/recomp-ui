if(NOT IS_ABSOLUTE "${SDL_SOURCE}" OR NOT EXISTS "${SDL_SOURCE}" OR NOT IS_ABSOLUTE "${SDL_DESTINATION}")
    message(FATAL_ERROR "Shared SDL staging requires the actual provider and absolute output directory")
endif()
get_filename_component(_sdl_name "${SDL_SOURCE}" NAME)
file(MAKE_DIRECTORY "${SDL_DESTINATION}")
file(COPY_FILE "${SDL_SOURCE}" "${SDL_DESTINATION}/${_sdl_name}" ONLY_IF_DIFFERENT)
if(SDL_SONAME AND NOT SDL_SONAME STREQUAL _sdl_name)
    # A versioned Unix library must retain the dependency name its consumer
    # imports. Never create a link over the actual file itself.
    file(CREATE_LINK "${_sdl_name}" "${SDL_DESTINATION}/${SDL_SONAME}"
        SYMBOLIC RESULT _sdl_link_result)
    if(NOT _sdl_link_result STREQUAL "0")
        message(FATAL_ERROR "Could not stage SDL dependency name: ${_sdl_link_result}")
    endif()
endif()
