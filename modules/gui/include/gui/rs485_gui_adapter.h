#pragma once

// İleri deklarasyonlar
namespace core {
struct PumpRuntimeState;
}

namespace recum12::gui {

class MainWindow;
class StatusMessageController;

// CORE PumpRuntimeState -> GUI görünümü
// Not: apply() her zaman GUI thread'inden çağrılmalı.
class Rs485GuiAdapter {
public:
    Rs485GuiAdapter(MainWindow& ui,
                    StatusMessageController& status);

    // CORE runtime state -> GUI görünümü
    void apply(const ::core::PumpRuntimeState& s);

private:
    MainWindow&             ui_;
    StatusMessageController& status_;
};

} // namespace recum12::gui
