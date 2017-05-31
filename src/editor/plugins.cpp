#include "editor/asset_browser.h"
#include "editor/ieditor_command.h"
#include "editor/platform_interface.h"
#include "editor/property_grid.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/array.h"
#include "engine/base_proxy_allocator.h"
#include "engine/binary_array.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/debug/debug.h"
#include "engine/engine.h"
#include "engine/fs/file_system.h"
#include "engine/fs/os_file.h"
#include "engine/iallocator.h"
#include "engine/iplugin.h"
#include "engine/json_serializer.h"
#include "engine/log.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/property_descriptor.h"
#include "engine/property_register.h"
#include "engine/resource_manager.h"
#include "engine/universe/universe.h"
#include "imgui/imgui.h"
#include "../js_script_manager.h"
#include "../js_script_system.h"
#include <cstdlib>


using namespace Lumix;


static const ComponentType JS_SCRIPT_TYPE = PropertyRegister::getComponentType("js_script");
static const ResourceType JS_SCRIPT_RESOURCE_TYPE("js_script");


namespace
{


struct PropertyGridPlugin LUMIX_FINAL : public PropertyGrid::IPlugin
{
	struct AddScriptCommand LUMIX_FINAL : public IEditorCommand
	{
		AddScriptCommand() {}


		explicit AddScriptCommand(WorldEditor& editor)
		{
			scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
		}


		bool execute() override
		{
			scr_index = scene->addScript(cmp);
			return true;
		}


		void undo() override { scene->removeScript(cmp, scr_index); }


		void serialize(JsonSerializer& serializer) override { serializer.serialize("component", cmp); }


		void deserialize(JsonSerializer& serializer) override
		{
			serializer.deserialize("component", cmp, INVALID_COMPONENT);
		}


		const char* getType() override { return "add_script"; }


		bool merge(IEditorCommand& command) override { return false; }


		JSScriptScene* scene;
		ComponentHandle cmp;
		int scr_index;
	};


	struct MoveScriptCommand LUMIX_FINAL : public IEditorCommand
	{
		explicit MoveScriptCommand(WorldEditor& editor)
			: blob(editor.getAllocator())
			, scr_index(-1)
			, cmp(INVALID_COMPONENT)
			, up(true)
		{
			scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
		}


		explicit MoveScriptCommand(IAllocator& allocator)
			: blob(allocator)
			, scene(nullptr)
			, scr_index(-1)
			, cmp(INVALID_COMPONENT)
			, up(true)
		{
		}


		bool execute() override
		{
			scene->moveScript(cmp, scr_index, up);
			return true;
		}


		void undo() override
		{
			scene->moveScript(cmp, up ? scr_index - 1 : scr_index + 1, !up);
		}


		void serialize(JsonSerializer& serializer) override
		{
			serializer.serialize("component", cmp);
			serializer.serialize("scr_index", scr_index);
			serializer.serialize("up", up);
		}


		void deserialize(JsonSerializer& serializer) override
		{
			serializer.deserialize("component", cmp, INVALID_COMPONENT);
			serializer.deserialize("scr_index", scr_index, 0);
			serializer.deserialize("up", up, false);
		}


		const char* getType() override { return "move_script"; }


		bool merge(IEditorCommand& command) override { return false; }


		OutputBlob blob;
		JSScriptScene* scene;
		ComponentHandle cmp;
		int scr_index;
		bool up;
	};


	struct RemoveScriptCommand LUMIX_FINAL : public IEditorCommand
	{
		explicit RemoveScriptCommand(WorldEditor& editor)
			: blob(editor.getAllocator())
			, scr_index(-1)
			, cmp(INVALID_COMPONENT)
		{
			scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
		}


		explicit RemoveScriptCommand(IAllocator& allocator)
			: blob(allocator)
			, scene(nullptr)
			, scr_index(-1)
			, cmp(INVALID_COMPONENT)
		{
		}


		bool execute() override
		{
			scene->serializeScript(cmp, scr_index, blob);
			scene->removeScript(cmp, scr_index);
			return true;
		}


		void undo() override
		{
			scene->insertScript(cmp, scr_index);
			InputBlob input(blob);
			scene->deserializeScript(cmp, scr_index, input);
		}


		void serialize(JsonSerializer& serializer) override
		{
			serializer.serialize("component", cmp);
			serializer.serialize("scr_index", scr_index);
		}


		void deserialize(JsonSerializer& serializer) override
		{
			serializer.deserialize("component", cmp, INVALID_COMPONENT);
			serializer.deserialize("scr_index", scr_index, 0);
		}


		const char* getType() override { return "remove_script"; }


		bool merge(IEditorCommand& command) override { return false; }

		OutputBlob blob;
		JSScriptScene* scene;
		ComponentHandle cmp;
		int scr_index;
	};


	struct SetPropertyCommand LUMIX_FINAL : public IEditorCommand
	{
		explicit SetPropertyCommand(WorldEditor& _editor)
			: property_name(_editor.getAllocator())
			, value(_editor.getAllocator())
			, old_value(_editor.getAllocator())
			, editor(_editor)
		{
		}


		SetPropertyCommand(WorldEditor& _editor,
			ComponentHandle cmp,
			int scr_index,
			const char* property_name,
			const char* val,
			IAllocator& allocator)
			: property_name(property_name, allocator)
			, value(val, allocator)
			, old_value(allocator)
			, component(cmp)
			, script_index(scr_index)
			, editor(_editor)
		{
			auto* scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
			if (property_name[0] == '-')
			{
				old_value = scene->getScriptPath(component, script_index).c_str();
			}
			else
			{
				char tmp[1024];
				tmp[0] = '\0';
				u32 prop_name_hash = crc32(property_name);
				scene->getPropertyValue(cmp, scr_index, property_name, tmp, lengthOf(tmp));
				old_value = tmp;
				return;
			}
		}


		bool execute() override
		{
			auto* scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
			if (property_name.length() > 0 && property_name[0] == '-')
			{
				scene->setScriptPath(component, script_index, Path(value.c_str()));
			}
			else
			{
				scene->setPropertyValue(component, script_index, property_name.c_str(), value.c_str());
			}
			return true;
		}


		void undo() override
		{
			auto* scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
			if (property_name.length() > 0 && property_name[0] == '-')
			{
				scene->setScriptPath(component, script_index, Path(old_value.c_str()));
			}
			else
			{
				scene->setPropertyValue(component, script_index, property_name.c_str(), old_value.c_str());
			}
		}


		void serialize(JsonSerializer& serializer) override
		{
			serializer.serialize("component", component);
			serializer.serialize("script_index", script_index);
			serializer.serialize("property_name", property_name.c_str());
			serializer.serialize("value", value.c_str());
			serializer.serialize("old_value", old_value.c_str());
		}


		void deserialize(JsonSerializer& serializer) override
		{
			serializer.deserialize("component", component, INVALID_COMPONENT);
			serializer.deserialize("script_index", script_index, 0);
			char buf[256];
			serializer.deserialize("property_name", buf, lengthOf(buf), "");
			property_name = buf;
			serializer.deserialize("value", buf, lengthOf(buf), "");
			value = buf;
			serializer.deserialize("old_value", buf, lengthOf(buf), "");
			old_value = buf;
		}


		const char* getType() override { return "set_script_property"; }


		bool merge(IEditorCommand& command) override
		{
			auto& cmd = static_cast<SetPropertyCommand&>(command);
			if (cmd.script_index == script_index && cmd.property_name == property_name)
			{
				//cmd.scene = scene;
				cmd.value = value;
				return true;
			}
			return false;
		}


		WorldEditor& editor;
		string property_name;
		string value;
		string old_value;
		ComponentHandle component;
		int script_index;
	};


	explicit PropertyGridPlugin(StudioApp& app)
		: m_app(app)
	{
	}


	void onGUI(PropertyGrid& grid, ComponentUID cmp) override
	{
		if (cmp.type != JS_SCRIPT_TYPE) return;

		auto* scene = static_cast<JSScriptScene*>(cmp.scene);
		auto& editor = *m_app.getWorldEditor();
		auto& allocator = editor.getAllocator();

		if (ImGui::Button("Add script"))
		{
			auto* cmd = LUMIX_NEW(allocator, AddScriptCommand);
			cmd->scene = scene;
			cmd->cmp = cmp.handle;
			editor.executeCommand(cmd);
		}

		for (int j = 0; j < scene->getScriptCount(cmp.handle); ++j)
		{
			char buf[MAX_PATH_LENGTH];
			copyString(buf, scene->getScriptPath(cmp.handle, j).c_str());
			StaticString<MAX_PATH_LENGTH + 20> header;
			PathUtils::getBasename(header.data, lengthOf(header.data), buf);
			if (header.empty()) header << j;
			header << "###" << j;
			if (ImGui::CollapsingHeader(header))
			{
				ImGui::PushID(j);
				if (ImGui::Button("Remove script"))
				{
					auto* cmd = LUMIX_NEW(allocator, RemoveScriptCommand)(allocator);
					cmd->cmp = cmp.handle;
					cmd->scr_index = j;
					cmd->scene = scene;
					editor.executeCommand(cmd);
					ImGui::PopID();
					break;
				}
				ImGui::SameLine();
				if (ImGui::Button("Up"))
				{
					auto* cmd = LUMIX_NEW(allocator, MoveScriptCommand)(allocator);
					cmd->cmp = cmp.handle;
					cmd->scr_index = j;
					cmd->scene = scene;
					cmd->up = true;
					editor.executeCommand(cmd);
					ImGui::PopID();
					break;
				}
				ImGui::SameLine();
				if (ImGui::Button("Down"))
				{
					auto* cmd = LUMIX_NEW(allocator, MoveScriptCommand)(allocator);
					cmd->cmp = cmp.handle;
					cmd->scr_index = j;
					cmd->scene = scene;
					cmd->up = false;
					editor.executeCommand(cmd);
					ImGui::PopID();
					break;
				}

				if (m_app.getAssetBrowser()->resourceInput(
						"Source", "src", buf, lengthOf(buf), JS_SCRIPT_RESOURCE_TYPE))
				{
					auto* cmd =
						LUMIX_NEW(allocator, SetPropertyCommand)(editor, cmp.handle, j, "-source", buf, allocator);
					editor.executeCommand(cmd);
				}
				for (int k = 0, kc = scene->getPropertyCount(cmp.handle, j); k < kc; ++k)
				{
					char buf[256];
					const char* property_name = scene->getPropertyName(cmp.handle, j, k);
					if (!property_name) continue;
					scene->getPropertyValue(cmp.handle, j, property_name, buf, lengthOf(buf));
					switch (scene->getPropertyType(cmp.handle, j, k))
					{
						case JSScriptScene::Property::BOOLEAN:
						{
							bool b = equalStrings(buf, "true");
							if (ImGui::Checkbox(property_name, &b))
							{
								auto* cmd = LUMIX_NEW(allocator, SetPropertyCommand)(
									editor, cmp.handle, j, property_name, b ? "true" : "false", allocator);
								editor.executeCommand(cmd);
							}
						}
						break;
						case JSScriptScene::Property::FLOAT:
						{
							float f = (float)atof(buf);
							if (ImGui::DragFloat(property_name, &f))
							{
								toCString(f, buf, sizeof(buf), 5);
								auto* cmd = LUMIX_NEW(allocator, SetPropertyCommand)(
									editor, cmp.handle, j, property_name, buf, allocator);
								editor.executeCommand(cmd);
							}
						}
						break;
						case JSScriptScene::Property::ENTITY:
						{
							Entity e;
							fromCString(buf, sizeof(buf), &e.index);
							if (grid.entityInput(property_name, StaticString<50>(property_name, cmp.handle.index), e))
							{
								toCString(e.index, buf, sizeof(buf));
								auto* cmd = LUMIX_NEW(allocator, SetPropertyCommand)(
									editor, cmp.handle, j, property_name, buf, allocator);
								editor.executeCommand(cmd);
							}
						}
						break;
						case JSScriptScene::Property::STRING:
						case JSScriptScene::Property::ANY:
							if (ImGui::InputText(property_name, buf, sizeof(buf)))
							{
								auto* cmd = LUMIX_NEW(allocator, SetPropertyCommand)(
									editor, cmp.handle, j, property_name, buf, allocator);
								editor.executeCommand(cmd);
							}
							break;
						case JSScriptScene::Property::RESOURCE:
						{
							ResourceType res_type = scene->getPropertyResourceType(cmp.handle, j, k);
							if (m_app.getAssetBrowser()->resourceInput(
									property_name, property_name, buf, lengthOf(buf), res_type))
							{
								auto* cmd = LUMIX_NEW(allocator, SetPropertyCommand)(
									editor, cmp.handle, j, property_name, buf, allocator);
								editor.executeCommand(cmd);
							}
						}
						break;
						default: ASSERT(false); break;
					}
				}
				if (auto* call = scene->beginFunctionCall(cmp.handle, j, "onGUI"))
				{
					scene->endFunctionCall();
				}
				ImGui::PopID();
			}
		}
	}

	StudioApp& m_app;
};


struct AssetBrowserPlugin : AssetBrowser::IPlugin
{
	explicit AssetBrowserPlugin(StudioApp& app)
		: m_app(app)
	{
		m_text_buffer[0] = 0;
	}


	bool acceptExtension(const char* ext, ResourceType type) const override
	{
		return type == JS_SCRIPT_RESOURCE_TYPE && equalStrings(".JS", ext);
	}


	bool onGUI(Resource* resource, ResourceType type) override
	{
		if (type != JS_SCRIPT_RESOURCE_TYPE) return false;

		auto* script = static_cast<JSScript*>(resource);

		if (m_text_buffer[0] == '\0')
		{
			copyString(m_text_buffer, script->getSourceCode());
		}
		ImGui::InputTextMultiline("Code", m_text_buffer, sizeof(m_text_buffer), ImVec2(0, 300));
		if (ImGui::Button("Save"))
		{
			auto& fs = m_app.getWorldEditor()->getEngine().getFileSystem();
			auto* file = fs.open(fs.getDefaultDevice(), resource->getPath(), FS::Mode::CREATE_AND_WRITE);

			if (!file)
			{
				g_log_warning.log("JS Script") << "Could not save " << resource->getPath();
				return true;
			}

			file->write(m_text_buffer, stringLength(m_text_buffer));
			fs.close(*file);
		}
		ImGui::SameLine();
		if (ImGui::Button("Open in external editor"))
		{
			m_app.getAssetBrowser()->openInExternalEditor(resource);
		}
		return true;
	}


	ResourceType getResourceType(const char* ext) override
	{
		if (equalStrings(ext, "JS")) return JS_SCRIPT_RESOURCE_TYPE;
		return INVALID_RESOURCE_TYPE;
	}


	void onResourceUnloaded(Resource*) override { m_text_buffer[0] = 0; }
	const char* getName() const override { return "JS Script"; }


	bool hasResourceManager(ResourceType type) const override { return type == JS_SCRIPT_RESOURCE_TYPE; }


	StudioApp& m_app;
	char m_text_buffer[8192];
};





struct ConsolePlugin LUMIX_FINAL : public StudioApp::IPlugin
{
	ConsolePlugin(StudioApp& _app)
		: app(_app)
		, opened(false)
		, autocomplete(_app.getWorldEditor()->getAllocator())
	{
		Action* action = LUMIX_NEW(app.getWorldEditor()->getAllocator(), Action)("JS Script Console", "script_console");
		action->func.bind<ConsolePlugin, &ConsolePlugin::toggleOpened>(this);
		action->is_selected.bind<ConsolePlugin, &ConsolePlugin::isOpened>(this);
		app.addWindowAction(action);
		buf[0] = '\0';
	}


	const char* getName() const override { return "script_console"; }


	bool isOpened() const { return opened; }
	void toggleOpened() { opened = !opened; }


	void autocompleteSubstep(duk_context* ctx, const char* str, ImGuiTextEditCallbackData *data)
	{
		char item[128];
		const char* next = str;
		char* c = item;
		while (*next != '.' && *next != '\0')
		{
			*c = *next;
			++next;
			++c;
		}
		*c = '\0';

		duk_enum(ctx, -1, DUK_ENUM_INCLUDE_SYMBOLS | DUK_ENUM_INCLUDE_NONENUMERABLE);
		while (duk_next(ctx, -1, 0))
		{
			/* [ ... enum key ] */
			const char* name = duk_to_string(ctx, -1);
			if (startsWith(name, item))
			{
				if (*next == '.' && next[1] == '\0')
				{
					duk_get_prop_string(ctx, -3, name);
					autocompleteSubstep(ctx, "", data);
					duk_pop(ctx);
				}
				else if (*next == '\0')
				{
					autocomplete.push(string(name, app.getWorldEditor()->getAllocator()));
				}
				else
				{
					duk_get_prop_string(ctx, -3, name);
					autocompleteSubstep(ctx, next + 1, data);
					duk_pop(ctx);
				}
			}
			duk_pop(ctx);
		}
		duk_pop(ctx);
	}


	static bool isWordChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}


	static int autocompleteCallback(ImGuiTextEditCallbackData *data)
	{
		auto* that = (ConsolePlugin*)data->UserData;
		WorldEditor& editor = *that->app.getWorldEditor();
		auto* scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(crc32("js_script")));
		if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			duk_context* ctx = scene->getGlobalContext();

			int start_word = data->CursorPos;
			char c = data->Buf[start_word - 1];
			while (start_word > 0 && (isWordChar(c) || c == '.'))
			{
				--start_word;
				c = data->Buf[start_word - 1];
			}
			char tmp[128];
			copyNString(tmp, lengthOf(tmp), data->Buf + start_word, data->CursorPos - start_word);

			that->autocomplete.clear();
			
			duk_push_global_object(ctx);
			that->autocompleteSubstep(ctx, tmp, data);
			duk_pop(ctx);
			if (!that->autocomplete.empty())
			{
				that->open_autocomplete = true;
				qsort(&that->autocomplete[0],
					that->autocomplete.size(),
					sizeof(that->autocomplete[0]),
					[](const void* a, const void* b) {
					const char* a_str = ((const string*)a)->c_str();
					const char* b_str = ((const string*)b)->c_str();
					return compareString(a_str, b_str);
				});
			}
		}
		else if (that->insert_value)
		{
			int start_word = data->CursorPos;
			char c = data->Buf[start_word - 1];
			while (start_word > 0 && (isWordChar(c)))
			{
				--start_word;
				c = data->Buf[start_word - 1];
			}
			data->InsertChars(data->CursorPos, that->insert_value + data->CursorPos - start_word);
			that->insert_value = nullptr;
		}
		return 0;
	}


	void onWindowGUI() override
	{
		auto* scene = (JSScriptScene*)app.getWorldEditor()->getUniverse()->getScene(JS_SCRIPT_TYPE);
		duk_context* context = scene->getGlobalContext();

		if (ImGui::BeginDock("JS Script console", &opened))
		{
			if (ImGui::Button("Execute"))
			{
				duk_push_string(context, buf);
				if (duk_peval(context) != 0)
				{
					const char* error = duk_safe_to_string(context, -1);
					g_log_error.log("JS Script") << error;
				}
				duk_pop(context);
			}
			ImGui::SameLine();
			if (ImGui::Button("Execute file"))
			{
				char tmp[MAX_PATH_LENGTH];
				if (PlatformInterface::getOpenFilename(tmp, MAX_PATH_LENGTH, "Scripts\0*.JS\0", nullptr))
				{
					FS::OsFile file;
					IAllocator& allocator = app.getWorldEditor()->getAllocator();
					if (file.open(tmp, FS::Mode::OPEN_AND_READ, allocator))
					{
						size_t size = file.size();
						Array<char> data(allocator);
						data.resize((int)size + 1);
						file.read(&data[0], size);
						file.close();
						data[(int)size] = '\0';
						duk_push_string(context, &data[0]);
						if (duk_peval(context) != 0)
						{
							const char* error = duk_safe_to_string(context, -1);
							g_log_error.log("JS Script") << error;
						}
						duk_pop(context);
					}
					else
					{
						g_log_error.log("JS Script") << "Failed to open file " << tmp;
					}
				}
			}
			if(insert_value) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextMultiline("",
				buf,
				lengthOf(buf),
				ImVec2(-1, -1),
				ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackCompletion,
				autocompleteCallback,
				this);

			if (open_autocomplete)
			{
				ImGui::OpenPopup("autocomplete");
				ImGui::SetNextWindowPos(ImGui::GetOsImePosRequest());
			}
			open_autocomplete = false;
			if (ImGui::BeginPopup("autocomplete"))
			{
				if (autocomplete.size() == 1)
				{
					insert_value = autocomplete[0].c_str();
				}
				if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow))) ++autocomplete_selected;
				if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow))) --autocomplete_selected;
				if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter))) insert_value = autocomplete[autocomplete_selected].c_str();
				if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) ImGui::CloseCurrentPopup();
				autocomplete_selected = Math::clamp(autocomplete_selected, 0, autocomplete.size() - 1);
				for (int i = 0, c = autocomplete.size(); i < c; ++i)
				{
					const char* value = autocomplete[i].c_str();
					if (ImGui::Selectable(value, autocomplete_selected == i))
					{
						insert_value = value;
					}
				}
				ImGui::EndPopup();
			}
		}
		ImGui::EndDock();
	}


	StudioApp& app;
	Array<string> autocomplete;
	bool opened;
	bool open_autocomplete = false;
	int autocomplete_selected = 1;
	const char* insert_value = nullptr;
	char buf[10 * 1024];
};


IEditorCommand* createAddScriptCommand(WorldEditor& editor)
{
	return LUMIX_NEW(editor.getAllocator(), PropertyGridPlugin::AddScriptCommand)(editor);
}


IEditorCommand* createSetPropertyCommand(WorldEditor& editor)
{
	return LUMIX_NEW(editor.getAllocator(), PropertyGridPlugin::SetPropertyCommand)(editor);
}


IEditorCommand* createRemoveScriptCommand(WorldEditor& editor)
{
	return LUMIX_NEW(editor.getAllocator(), PropertyGridPlugin::RemoveScriptCommand)(editor);
}


struct AddComponentPlugin LUMIX_FINAL : public StudioApp::IAddComponentPlugin
{
	AddComponentPlugin(StudioApp& _app)
		: app(_app)
	{
	}


	void onGUI(bool create_entity, bool) override
	{
		ImGui::SetNextWindowSize(ImVec2(300, 300));
		if (!ImGui::BeginMenu(getLabel())) return;
		char buf[MAX_PATH_LENGTH];
		auto* asset_browser = app.getAssetBrowser();
		bool new_created = false;
		if (ImGui::Selectable("New"))
		{
			char full_path[MAX_PATH_LENGTH];
			if (PlatformInterface::getSaveFilename(full_path, lengthOf(full_path), "JS script\0*.js\0", "js"))
			{
				FS::OsFile file;
				WorldEditor& editor = *app.getWorldEditor();
				IAllocator& allocator = editor.getAllocator();
				if (file.open(full_path, FS::Mode::CREATE_AND_WRITE, allocator))
				{
					new_created = true;
					editor.makeRelative(buf, lengthOf(buf), full_path);
					file.close();
				}
				else
				{
					g_log_error.log("JS Script") << "Failed to create " << buf;
				}
			}
		}
		bool create_empty = ImGui::Selectable("Empty", false);

		if (asset_browser->resourceList(buf, lengthOf(buf), JS_SCRIPT_RESOURCE_TYPE, 0) || create_empty || new_created)
		{
			WorldEditor& editor = *app.getWorldEditor();
			if (create_entity)
			{
				Entity entity = editor.addEntity();
				editor.selectEntities(&entity, 1);
			}
			if (editor.getSelectedEntities().empty()) return;
			Entity entity = editor.getSelectedEntities()[0];

			if (!editor.getUniverse()->hasComponent(entity, JS_SCRIPT_TYPE))
			{
				editor.addComponent(JS_SCRIPT_TYPE);
			}

			IAllocator& allocator = editor.getAllocator();
			auto* cmd = LUMIX_NEW(allocator, PropertyGridPlugin::AddScriptCommand);

			auto* script_scene = static_cast<JSScriptScene*>(editor.getUniverse()->getScene(JS_SCRIPT_TYPE));
			ComponentHandle cmp = editor.getUniverse()->getComponent(entity, JS_SCRIPT_TYPE).handle;
			cmd->scene = script_scene;
			cmd->cmp = cmp;
			editor.executeCommand(cmd);

			if (!create_empty)
			{
				int scr_count = script_scene->getScriptCount(cmp);
				auto* set_source_cmd = LUMIX_NEW(allocator, PropertyGridPlugin::SetPropertyCommand)(
					editor, cmp, scr_count - 1, "-source", buf, allocator);
				editor.executeCommand(set_source_cmd);
			}

			ImGui::CloseCurrentPopup();
		}
		ImGui::EndMenu();
	}


	const char* getLabel() const override 
	{
		return "JS Script";
	}


	StudioApp& app;
};


struct EditorPlugin : public WorldEditor::Plugin
{
	EditorPlugin(WorldEditor& _editor)
		: editor(_editor)
	{
	}


	bool showGizmo(ComponentUID cmp) override
	{
		if (cmp.type == JS_SCRIPT_TYPE)
		{
			auto* scene = static_cast<JSScriptScene*>(cmp.scene);
			int count = scene->getScriptCount(cmp.handle);
			for (int i = 0; i < count; ++i)
			{
				if (auto* call = scene->beginFunctionCall(cmp.handle, i, "onDrawGizmo"))
				{
					scene->endFunctionCall();
				}
			}
			return true;
		}
		return false;
	}


	WorldEditor& editor;
};


} // anonoymous namespace


LUMIX_STUDIO_ENTRY(lumixengine_js)
{
	auto& editor = *app.getWorldEditor();
	auto* cmp_plugin = LUMIX_NEW(editor.getAllocator(), AddComponentPlugin)(app);
	app.registerComponent("js_script", *cmp_plugin);

	editor.registerEditorCommandCreator("add_script", createAddScriptCommand);
	editor.registerEditorCommandCreator("remove_script", createRemoveScriptCommand);
	editor.registerEditorCommandCreator("set_script_property", createSetPropertyCommand);
	auto* editor_plugin = LUMIX_NEW(editor.getAllocator(), EditorPlugin)(editor);
	editor.addPlugin(*editor_plugin);

	auto* plugin = LUMIX_NEW(editor.getAllocator(), PropertyGridPlugin)(app);
	app.getPropertyGrid()->addPlugin(*plugin);

	auto* asset_browser_plugin = LUMIX_NEW(editor.getAllocator(), AssetBrowserPlugin)(app);
	app.getAssetBrowser()->addPlugin(*asset_browser_plugin);

	auto* console_plugin = LUMIX_NEW(editor.getAllocator(), ConsolePlugin)(app);
	app.addPlugin(*console_plugin);
}


