/******************************************************************************
    Streaming checklist — a very simple rich-text editor (bullet and
    numbered lists only) that pops up before a stream actually goes live,
    so the user can review a pre-flight checklist. Content persists between
    streams. Gates StartStreaming(): the caller only proceeds if the dialog
    is accepted (Start Streaming), not if it's cancelled or closed.
******************************************************************************/

#pragma once

#include <QDialog>
#include <QTextListFormat>

class QTextEdit;

class StreamingChecklistDialog : public QDialog {
	Q_OBJECT

public:
	explicit StreamingChecklistDialog(QWidget *parent = nullptr);

private:
	QTextEdit *editor;

	void ToggleList(QTextListFormat::Style style);
	void Save();
	void Load();
};
