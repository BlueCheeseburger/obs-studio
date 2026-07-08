#include "StreamingChecklistDialog.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextList>
#include <QToolButton>
#include <QVBoxLayout>

#include "moc_StreamingChecklistDialog.cpp"

static QString ChecklistFilePath()
{
	char path[512];
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/streaming_checklist.html") <= 0)
		return QString();
	return QT_UTF8(path);
}

StreamingChecklistDialog::StreamingChecklistDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QTStr("StreamingChecklist.Title"));
	resize(480, 520);

	auto *layout = new QVBoxLayout(this);

	auto *hint = new QLabel(QTStr("StreamingChecklist.Hint"), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	auto *toolbar = new QHBoxLayout();
	auto *bulletButton = new QToolButton(this);
	bulletButton->setText(QStringLiteral("• ") + QTStr("StreamingChecklist.Bullets"));
	auto *numberButton = new QToolButton(this);
	numberButton->setText(QStringLiteral("1. ") + QTStr("StreamingChecklist.Numbered"));
	toolbar->addWidget(bulletButton);
	toolbar->addWidget(numberButton);
	toolbar->addStretch(1);
	layout->addLayout(toolbar);

	editor = new QTextEdit(this);
	editor->setAcceptRichText(true);
	layout->addWidget(editor, 1);

	connect(bulletButton, &QToolButton::clicked, this,
		[this]() { ToggleList(QTextListFormat::ListDisc); });
	connect(numberButton, &QToolButton::clicked, this,
		[this]() { ToggleList(QTextListFormat::ListDecimal); });

	auto *buttons = new QDialogButtonBox(this);
	auto *startButton = buttons->addButton(QTStr("StreamingChecklist.StartStreaming"), QDialogButtonBox::AcceptRole);
	buttons->addButton(QTStr("Cancel"), QDialogButtonBox::RejectRole);
	startButton->setDefault(true);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	/* Persist edits regardless of whether the user starts streaming or
	 * cancels — this is a reusable checklist, not a one-shot form. */
	connect(this, &QDialog::finished, this, [this](int) { Save(); });

	Load();
	editor->setFocus();
}

void StreamingChecklistDialog::ToggleList(QTextListFormat::Style style)
{
	QTextCursor cursor = editor->textCursor();
	cursor.beginEditBlock();

	QTextList *list = cursor.currentList();
	if (list && list->format().style() == style) {
		/* already this list type: lift the selected blocks back out */
		QTextBlockFormat bf = cursor.blockFormat();
		bf.setIndent(0);
		cursor.setBlockFormat(bf);
		list->remove(cursor.block());
	} else {
		QTextListFormat fmt;
		fmt.setStyle(style);
		cursor.createList(fmt);
	}

	cursor.endEditBlock();
	editor->setFocus();
}

void StreamingChecklistDialog::Load()
{
	QString path = ChecklistFilePath();
	if (path.isEmpty())
		return;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

	editor->setHtml(QString::fromUtf8(f.readAll()));
}

void StreamingChecklistDialog::Save()
{
	QString path = ChecklistFilePath();
	if (path.isEmpty())
		return;

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
		return;

	f.write(editor->toHtml().toUtf8());
}
