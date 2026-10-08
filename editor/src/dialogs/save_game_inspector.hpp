#pragma once

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QTreeWidget;

namespace ox::editor {

class EditorContext;
class SearchField;

// Tools → Save Game Inspector: the project's save slots in the user directory (user://saves, SaveGameSystem::
// listSlots: display name, level, play time, date, size, corrupted/backup flags) and the selected slot decoded
// to JSON with the oxdump logic (serial::binaryToJson), read-only. Any .oxsave file can be opened too.
class SaveGameInspector : public QDialog {
    Q_OBJECT
public:
    explicit SaveGameInspector(EditorContext* ctx, QWidget* parent = nullptr);

    void refresh();
    // Shows a save file (slot file or any .oxsave). Returns false when it cannot be decoded.
    bool showFile(const QString& path);
    [[nodiscard]] QTreeWidget* slots() const { return m_slots; }
    [[nodiscard]] QString json() const;
    [[nodiscard]] QString savesDir() const;

private:
    void showSlot(const QString& slot, const QString& path);

    EditorContext* m_ctx;
    QTreeWidget* m_slots;
    QPlainTextEdit* m_json;
    QLabel* m_header;
    QLabel* m_dirLabel;
    SearchField* m_find;
};

} // namespace ox::editor
