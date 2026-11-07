void App::draw_console() {
    if(!show_console_)return;
    ImGui::SetNextWindowSize(ImVec2(850,260),ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0,0,0,1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,1));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(6,2));
    bool open=show_console_;
    if(ImGui::Begin("Terminal###FoxTerminal",&show_console_)) {
        static ImGuiTextFilter filter;
        static bool follow=true,paused=false;
        static int level=0;
        static unsigned long long revision=0;
        static std::vector<app_log::Line> lines;
        static std::vector<int> visible;
        bool dirty=false;
        if(ImGui::Button("Clear")){auto& s=app_log::store();std::scoped_lock lock(s.mutex);s.lines.clear();++s.revision;lines.clear();revision=0;dirty=true;}
        ImGui::SameLine(); bool copy=ImGui::Button("Copy");
        ImGui::SameLine(); ImGui::Checkbox("Follow",&follow);
        ImGui::SameLine(); ImGui::Checkbox("Pause",&paused);
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize()*8);
        dirty|=ImGui::Combo("##logLevel",&level,"All levels\0Info +\0Warnings +\0Errors\0");
        ImGui::SameLine();dirty|=filter.Draw("##logFilter",std::max(80.f,ImGui::GetContentRegionAvail().x));
        ImGui::SetItemTooltip("Filter logs (comma-separated terms, -term to exclude)");
        if(!paused) {
            auto& s=app_log::store();std::scoped_lock lock(s.mutex);
            if(revision!=s.revision){lines.assign(s.lines.begin(),s.lines.end());revision=s.revision;dirty=true;}
        }
        const int minimum=level?level+1:0;
        if(dirty){visible.clear();for(int i=0;i<int(lines.size());++i)if(lines[i].level>=minimum && filter.PassFilter(lines[i].text.c_str()))visible.push_back(i);}
        static const char* labels[]={"DEBUG","TRACE","INFO ","WARN ","ERROR"};
        if(copy){std::string text;for(int i:visible){const auto& l=lines[i];text+=l.time+"  "+labels[std::clamp(l.level,0,4)]+"  "+l.text+'\n';}ImGui::SetClipboardText(text.c_str());}
        ImGui::Separator();
        ImGui::BeginChild("logLines",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
        if(ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel>0)follow=false;
        const ImVec4 colors[]={ImVec4(.48f,.52f,.58f,1),ImVec4(.56f,.48f,.75f,1),ImVec4(.43f,.79f,.91f,1),ImVec4(1,.73f,.32f,1),ImVec4(1,.35f,.38f,1)};
        ImGuiListClipper clipper;clipper.Begin(int(visible.size()));
        while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){
            const auto& l=lines[visible[row]];const int severity=std::clamp(l.level,0,4);
            ImGui::TextColored(ImVec4(.38f,.42f,.46f,1),"%s",l.time.c_str());ImGui::SameLine();
            ImGui::TextColored(colors[severity],"%s",labels[severity]);ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,severity>=3?colors[severity]:ImVec4(.79f,.82f,.85f,1));
            ImGui::TextUnformatted(l.text.c_str());ImGui::PopStyleColor();
        }
        if(follow)ImGui::SetScrollY(ImGui::GetScrollMaxY());
        ImGui::EndChild();
    }
    ImGui::End();ImGui::PopStyleVar();ImGui::PopStyleColor(2);
    if(open!=show_console_)save_settings();
}
