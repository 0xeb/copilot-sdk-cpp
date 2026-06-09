# Examples for copilot-sdk-cpp

# Basic chat example
add_executable(basic_chat basic_chat.cpp)
target_link_libraries(basic_chat PRIVATE copilot_sdk_cpp)
set_target_properties(basic_chat PROPERTIES FOLDER "Examples")

# Streaming example
add_executable(streaming streaming.cpp)
target_link_libraries(streaming PRIVATE copilot_sdk_cpp)
set_target_properties(streaming PROPERTIES FOLDER "Examples")

# Tools example
add_executable(tools tools.cpp)
target_link_libraries(tools PRIVATE copilot_sdk_cpp)
set_target_properties(tools PROPERTIES FOLDER "Examples")

# BYOK (Bring Your Own Key) example
add_executable(byok byok.cpp)
target_link_libraries(byok PRIVATE copilot_sdk_cpp)
set_target_properties(byok PROPERTIES FOLDER "Examples")

# Resume with tools example
add_executable(resume_with_tools resume_with_tools.cpp)
target_link_libraries(resume_with_tools PRIVATE copilot_sdk_cpp)
set_target_properties(resume_with_tools PROPERTIES FOLDER "Examples")

# Permission callback example
add_executable(permission_callback permission_callback.cpp)
target_link_libraries(permission_callback PRIVATE copilot_sdk_cpp)
set_target_properties(permission_callback PROPERTIES FOLDER "Examples")

# System prompt configuration example
add_executable(system_prompt system_prompt.cpp)
target_link_libraries(system_prompt PRIVATE copilot_sdk_cpp)
set_target_properties(system_prompt PROPERTIES FOLDER "Examples")

# Custom agents example
add_executable(custom_agents custom_agents.cpp)
target_link_libraries(custom_agents PRIVATE copilot_sdk_cpp)
set_target_properties(custom_agents PROPERTIES FOLDER "Examples")

# MCP servers example
add_executable(mcp_servers mcp_servers.cpp)
target_link_libraries(mcp_servers PRIVATE copilot_sdk_cpp)
set_target_properties(mcp_servers PROPERTIES FOLDER "Examples")

# File attachments example
add_executable(attachments attachments.cpp)
target_link_libraries(attachments PRIVATE copilot_sdk_cpp)
set_target_properties(attachments PROPERTIES FOLDER "Examples")

# Fluent tool builder example
add_executable(fluent_tools fluent_tools.cpp)
target_link_libraries(fluent_tools PRIVATE copilot_sdk_cpp)
set_target_properties(fluent_tools PROPERTIES FOLDER "Examples")

# Compaction events monitoring example
add_executable(compaction_events compaction_events.cpp)
target_link_libraries(compaction_events PRIVATE copilot_sdk_cpp)
set_target_properties(compaction_events PROPERTIES FOLDER "Examples")

# List models with vision capabilities example
add_executable(list_models list_models.cpp)
target_link_libraries(list_models PRIVATE copilot_sdk_cpp)
set_target_properties(list_models PROPERTIES FOLDER "Examples")

# Tool execution progress monitoring example
add_executable(tool_progress tool_progress.cpp)
target_link_libraries(tool_progress PRIVATE copilot_sdk_cpp)
set_target_properties(tool_progress PROPERTIES FOLDER "Examples")

# Hooks system example (v0.1.23)
add_executable(hooks hooks.cpp)
target_link_libraries(hooks PRIVATE copilot_sdk_cpp)
set_target_properties(hooks PROPERTIES FOLDER "Examples")

# User input handler example (v0.1.23)
add_executable(user_input user_input.cpp)
target_link_libraries(user_input PRIVATE copilot_sdk_cpp)
set_target_properties(user_input PROPERTIES FOLDER "Examples")

# Reasoning effort example (v0.1.23)
add_executable(reasoning_effort reasoning_effort.cpp)
target_link_libraries(reasoning_effort PRIVATE copilot_sdk_cpp)
set_target_properties(reasoning_effort PROPERTIES FOLDER "Examples")

# Instruction directories example (v0.1.49 - PR #1190)
add_executable(instruction_directories instruction_directories.cpp)
target_link_libraries(instruction_directories PRIVATE copilot_sdk_cpp)
set_target_properties(instruction_directories PROPERTIES FOLDER "Examples")

# Remote session example (v0.1.49 - PR #1295, Mission Control integration)
add_executable(remote_session remote_session.cpp)
target_link_libraries(remote_session PRIVATE copilot_sdk_cpp)
set_target_properties(remote_session PROPERTIES FOLDER "Examples")
