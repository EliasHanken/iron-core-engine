# gamecommon — shared, OPTIONAL gameplay building blocks for the sample games.
# These are NOT core engine: gameplay systems like health, weapon cooldowns, and
# inventories are genre-specific and vary widely between games, so they live
# game-side. Games opt in by linking `gamecommon`; the core `ironcore` library
# stays limited to generic engine mechanisms (rendering, physics/collision,
# audio, input, reflection, serialization, ...).
#
# Include as: #include "common/Health.h"  (the include root is the games/ dir).
add_library(gamecommon STATIC
  Inventory.cpp)
target_include_directories(gamecommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(gamecommon PUBLIC ironcore)
