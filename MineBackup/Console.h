#pragma once
#ifndef _CONSOLE_
#define _CONSOLE_

#include "imgui/imgui.h"
#include "AppState.h"
#ifdef _WIN32
#include "Platform_win.h"
#else
#include "Platform_linux.h"
#endif
#include "i18n.h"
#include "text_to_text.h"

#include <mutex>
#include <fstream>
#include <filesystem>
extern bool isSilence;
struct Console
{
	char                  InputBuf[CONSTANT1];
	ImVector<char*>       Items;
	ImVector<const char*> Commands;
	ImVector<char*>       History;
	int                   HistoryPos;    // -1: new line, 0..History.Size-1 browsing history.
	ImGuiTextFilter       Filter;
	bool                  AutoScroll;
	bool                  ScrollToBottom;
	std::mutex				  logMutex; //用于保护日志内容的互斥锁 

	Console()
	{
		ClearLog();
		memset(InputBuf, 0, sizeof(InputBuf));
		HistoryPos = -1;

		// "CLASSIFY" is here to provide the test case where "C"+[tab] completes to "CL" and display multiple matches.
		Commands.push_back("HELP");
		Commands.push_back("HISTORY");
		Commands.push_back("CLEAR");
		Commands.push_back("BACKUP");
		Commands.push_back("BACKUP_MODS");
		Commands.push_back("RESTORE");
		Commands.push_back("SET_CONFIG");
		Commands.push_back("GET_CONFIG");
		Commands.push_back("LIST_BACKUPS");
		Commands.push_back("LIST_WORLDS");
		Commands.push_back("LIST_CONFIGS");
		AutoScroll = true;                  //自动滚动好呀
		ScrollToBottom = false;             //不用滚动条，但可以鼠标滚
	}
	~Console()
	{
		ClearLog();
		for (int i = 0; i < History.Size; i++)
			ImGui::MemFree(History[i]);
	}

	// Portable helpers
	static int   Stricmp(const char* s1, const char* s2) { int d; while ((d = toupper(*s2) - toupper(*s1)) == 0 && *s1) { s1++; s2++; } return d; }
	static int   Strnicmp(const char* s1, const char* s2, int n) { int d = 0; while (n > 0 && (d = toupper(*s2) - toupper(*s1)) == 0 && *s1) { s1++; s2++; n--; } return d; }
	static char* Strdup(const char* s) { IM_ASSERT(s); size_t len = strlen(s) + 1; void* buf = ImGui::MemAlloc(len); IM_ASSERT(buf); return (char*)memcpy(buf, (const void*)s, len); }
	static void  Strtrim(char* s) { char* str_end = s + strlen(s); while (str_end > s && str_end[-1] == ' ') str_end--; *str_end = 0; }

	void    ClearLog()
	{
		std::lock_guard<std::mutex> lock(logMutex);//加锁
		for (int i = 0; i < Items.Size; i++)
			ImGui::MemFree(Items[i]);
		Items.clear();
	}

	//显示消息
	void    AddLog(const char* fmt, ...) IM_FMTARGS(2)
	{
		if (isSilence) return;
		std::lock_guard<std::mutex> lock(logMutex);
		// FIXME-OPT
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
		buf[IM_ARRAYSIZE(buf) - 1] = 0;
		va_end(args);
		Items.push_back(Strdup(buf));
	}

	void    Draw(const char* title, bool* p_open)
	{
		ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin(title, p_open))
		{
			ImGui::End();
			return;
		}

		// As a specific feature guaranteed by the library, after calling Begin() the last Item represent the title bar.
		// So e.g. IsItemHovered() will return true when hovering the title bar.
		// Here we create a context menu only available from the title bar.(暂时无用
		/*if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem(u8"关闭"))
				*p_open = false;
			ImGui::EndPopup();
		}*/

		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT1"));
		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT2"));

		if (ImGui::SmallButton(L("BUTTON_CLEAR"))) { ClearLog(); }
		ImGui::SameLine();
		bool copy_to_clipboard = ImGui::SmallButton(L("BUTTON_COPY"));
		ImGui::Separator();

		if (ImGui::BeginPopup(L("BUTTON_OPTIONS")))
		{
			ImGui::Checkbox(L("CONSOLE_AUTO_SCROLL"), &AutoScroll);
			ImGui::EndPopup();
		}

		ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_Tooltip);
		if (ImGui::Button(L("BUTTON_OPTIONS")))
			ImGui::OpenPopup(L("BUTTON_OPTIONS"));
		ImGui::SameLine();
		Filter.Draw(L("CONSOLE_FILTER_HINT"), 180);
		ImGui::Separator();

		// Reserve enough left-over height for 1 separator + 1 input text
		const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
		if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
			if (copy_to_clipboard)
				ImGui::LogToClipboard();
			for (const char* item : Items)
			{
				if (!Filter.PassFilter(item))
					continue;

				// Normally you would store more information in your item than just a string.
				// (e.g. make Items[] an array of structure, store color/type etc.)
				ImVec4 color;
				bool has_color = false;
				if (strstr(item, "[Error]")) { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); has_color = true; }
				else if (strncmp(item, "# ", 2) == 0 || strncmp(item, "[Info] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				else if (strncmp(item, as_utf8(u8"[提示] "), 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				if (has_color)
					ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(item);
				if (has_color)
					ImGui::PopStyleColor();
			}
			if (copy_to_clipboard)
				ImGui::LogFinish();

			// Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
			// Using a scrollbar or mouse-wheel will take away from the bottom edge.
			if (ScrollToBottom || (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
				ImGui::SetScrollHereY(1.0f);
			ScrollToBottom = false;

			ImGui::PopStyleVar();
		}
		ImGui::EndChild();
		ImGui::Separator();

		// Command-line
		bool reclaim_focus = false;
		ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
		if (ImGui::InputText(L("CONSOLE_INPUT_LABEL"), InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
		{
			char* s = InputBuf;
			Strtrim(s);
			if (s[0])
				ExecCommand(s);
			strcpy_s(s, strlen(s) + 1, "");
			reclaim_focus = true;
		}

		// Auto-focus on window apparition
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget

		ImGui::End();
	}


	void DrawEmbedded()
	{
		// NOTE: this code is the inner body of the original Console::Draw,
		//       adapted to run *inside* an existing ImGui window/child.
		//       It intentionally DOES NOT call ImGui::SetNextWindowSize/ImGui::Begin/ImGui::End.

		ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);


		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT1"));
		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT2"));

		if (ImGui::SmallButton(L("BUTTON_CLEAR"))) { ClearLog(); }
		ImGui::SameLine();
		bool copy_to_clipboard = ImGui::SmallButton(L("BUTTON_COPY"));
		ImGui::SameLine();
		if (ImGui::SmallButton(L("BUTTON_EXPORT_LOG"))) {
			std::ofstream out("console_log.txt", std::ios::out | std::ios::trunc);
			if (!out.is_open()) return;
			for (int i = 0; i < Items.Size; ++i)
			{
				out << Items[i] << std::endl;
			}
			out.close();
			// 自动打开日志所在目录 并选中该文件
			OpenFolderWithFocus(std::filesystem::current_path().wstring(), L"/select,\"console_log.txt\"");
		}
		ImGui::Separator();

		if (ImGui::BeginPopup(L("BUTTON_OPTIONS")))
		{
			ImGui::Checkbox(L("CONSOLE_AUTO_SCROLL"), &AutoScroll);
			ImGui::EndPopup();
		}

		ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_Tooltip);
		if (ImGui::Button(L("BUTTON_OPTIONS")))
			ImGui::OpenPopup(L("BUTTON_OPTIONS"));
		ImGui::SameLine();
		Filter.Draw(L("CONSOLE_FILTER_HINT"), 180);
		ImGui::Separator();

		// Reserve enough left-over height for 1 separator + 1 input text
		const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
		if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar))
		{

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
			if (copy_to_clipboard)
				ImGui::LogToClipboard();
			for (const char* item : Items)
			{
				if (!Filter.PassFilter(item))
					continue;

				// Normally you would store more information in your item than just a string.
				// (e.g. make Items[] an array of structure, store color/type etc.)
				ImVec4 color;
				bool has_color = false;
				if (strstr(item, "[Error]")) { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); has_color = true; }
				else if (strncmp(item, "# ", 2) == 0 || strncmp(item, "[Info] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				else if (strncmp(item, as_utf8(u8"[提示] "), 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				if (has_color)
					ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(item);
				if (has_color)
					ImGui::PopStyleColor();
			}
			if (copy_to_clipboard)
				ImGui::LogFinish();

			// Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
			// Using a scrollbar or mouse-wheel will take away from the bottom edge.
			if (ScrollToBottom || (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
				ImGui::SetScrollHereY(1.0f);
			ScrollToBottom = false;

			ImGui::PopStyleVar();
		}
		ImGui::EndChild();
		ImGui::Separator();

		// Command-line
		bool reclaim_focus = false;
		ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
		if (ImGui::InputText(L("CONSOLE_INPUT_LABEL"), InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
		{
			char* s = InputBuf;
			Strtrim(s);
			if (s[0])
				ExecCommand(s);
			strcpy_s(s, strlen(s) + 1, "");//被要求从strcpy改成strcpy_s，这样中间要加个长度参数才不报错……
			reclaim_focus = true;
		}

		// Auto-focus on window apparition
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget
	}

	void  ExecCommand(const char* command_line);

	// In C++11 you'd be better off using lambdas for this sort of forwarding callbacks
	static int TextEditCallbackStub(ImGuiInputTextCallbackData* data)
	{
		Console* console = (Console*)data->UserData;
		return console->TextEditCallback(data);
	}

	int     TextEditCallback(ImGuiInputTextCallbackData* data)
	{
		switch (data->EventFlag)
		{
		case ImGuiInputTextFlags_CallbackCompletion:
		{
			const char* word_end = data->Buf + data->CursorPos;
			const char* word_start = word_end;
			while (word_start > data->Buf)
			{
				const char c = word_start[-1];
				if (c == ' ' || c == '\t' || c == ',' || c == ';')
					break;
				word_start--;
			}

			// Build a list of candidates
			ImVector<const char*> candidates;
			for (int i = 0; i < Commands.Size; i++)
				if (Strnicmp(Commands[i], word_start, (int)(word_end - word_start)) == 0)
					candidates.push_back(Commands[i]);

			if (candidates.Size == 0)
			{
				// No match
				AddLog(L("CONSOLE_CMD_MATCH_NONE"), (int)(word_end - word_start), word_start);
			}
			else if (candidates.Size == 1)
			{
				// Single match. Delete the beginning of the word and replace it entirely so we've got nice casing.
				data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
				data->InsertChars(data->CursorPos, candidates[0]);
				data->InsertChars(data->CursorPos, " ");
			}
			else
			{
				// 按Tab自动筛选
				int match_len = (int)(word_end - word_start);
				for (;;)
				{
					int c = 0;
					bool all_candidates_matches = true;
					for (int i = 0; i < candidates.Size && all_candidates_matches; i++)
						if (i == 0)
							c = toupper(candidates[i][match_len]);
						else if (c == 0 || c != toupper(candidates[i][match_len]))
							all_candidates_matches = false;
					if (!all_candidates_matches)
						break;
					match_len++;
				}

				if (match_len > 0)
				{
					data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
					data->InsertChars(data->CursorPos, candidates[0], candidates[0] + match_len);
				}

				// List matches
				AddLog(L("CONSOLE_CMD_MATCH_MULTIPLE"));
				for (int i = 0; i < candidates.Size; i++)
					AddLog("- %s\n", candidates[i]);
			}

			break;
		}
		case ImGuiInputTextFlags_CallbackHistory:
		{
			// Example of HISTORY
			const int prev_history_pos = HistoryPos;
			if (data->EventKey == ImGuiKey_UpArrow)
			{
				if (HistoryPos == -1)
					HistoryPos = History.Size - 1;
				else if (HistoryPos > 0)
					HistoryPos--;
			}
			else if (data->EventKey == ImGuiKey_DownArrow)
			{
				if (HistoryPos != -1)
					if (++HistoryPos >= History.Size)
						HistoryPos = -1;
			}

			// A better implementation would preserve the data on the current input line along with cursor position.
			if (prev_history_pos != HistoryPos)
			{
				const char* history_str = (HistoryPos >= 0) ? History[HistoryPos] : "";
				data->DeleteChars(0, data->BufTextLen);
				data->InsertChars(0, history_str);
			}
		}
		}
		return 0;
	}
};
extern Console console;
#endif // _CONSOLE_