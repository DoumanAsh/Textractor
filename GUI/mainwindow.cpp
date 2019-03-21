#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "defs.h"
#include "extenwindow.h"
#include "host/util.h"
#include <shellapi.h>
#include <winhttp.h>
#include <QFormLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QTextStream>

extern const char* ATTACH;
extern const char* LAUNCH;
extern const char* DETACH;
extern const char* ADD_HOOK;
extern const char* SAVE_HOOKS;
extern const char* SETTINGS;
extern const char* EXTENSIONS;
extern const char* SELECT_PROCESS;
extern const char* ATTACH_INFO;
extern const char* SEARCH_GAME;
extern const char* PROCESSES;
extern const char* CODE_INFODUMP;
extern const char* SAVE_SETTINGS;
extern const char* USE_JP_LOCALE;
extern const char* FILTER_REPETITION;
extern const char* DEFAULT_CODEPAGE;
extern const char* FLUSH_DELAY;
extern const char* MAX_BUFFER_SIZE;
extern const wchar_t* ABOUT;
extern const wchar_t* CL_OPTIONS;
extern const wchar_t* UPDATE_AVAILABLE;
extern const wchar_t* LAUNCH_FAILED;
extern const wchar_t* INVALID_CODE;

MainWindow::MainWindow(QWidget *parent) :
	QMainWindow(parent),
	ui(new Ui::MainWindow),
	extenWindow(new ExtenWindow(this))
{
	ui->setupUi(this);
	for (auto[text, slot] : Array<std::tuple<QString, void(MainWindow::*)()>>{
		{ ATTACH, &MainWindow::AttachProcess },
		{ LAUNCH, &MainWindow::LaunchProcess },
		{ DETACH, &MainWindow::DetachProcess },
		{ ADD_HOOK, &MainWindow::AddHook },
		{ SAVE_HOOKS, &MainWindow::SaveHooks },
		{ SETTINGS, &MainWindow::Settings },
		{ EXTENSIONS, &MainWindow::Extensions }
	})
	{
		auto button = new QPushButton(ui->processFrame);
		connect(button, &QPushButton::clicked, this, slot);
		button->setText(text);
		ui->processLayout->addWidget(button);
	}
	ui->processLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));

	connect(ui->ttCombo, qOverload<int>(&QComboBox::activated), this, &MainWindow::ViewThread);
	connect(ui->textOutput, &QPlainTextEdit::selectionChanged, [this] { if (!(QApplication::mouseButtons() & Qt::LeftButton)) ui->textOutput->copy(); });

	QSettings settings(CONFIG_FILE, QSettings::IniFormat);
	if (settings.contains(WINDOW)) setGeometry(settings.value(WINDOW).toRect());
	TextThread::filterRepetition = settings.value(FILTER_REPETITION, TextThread::filterRepetition).toBool();
	TextThread::flushDelay = settings.value(FLUSH_DELAY, TextThread::flushDelay).toInt();
	TextThread::maxBufferSize = settings.value(MAX_BUFFER_SIZE, TextThread::maxBufferSize).toInt();
	Host::defaultCodepage = settings.value(DEFAULT_CODEPAGE, Host::defaultCodepage).toInt();

	Host::Start(
		[this](DWORD processId) { ProcessConnected(processId); },
		[this](DWORD processId) { ProcessDisconnected(processId); },
		[this](TextThread& thread) { ThreadAdded(thread); },
		[this](TextThread& thread) { ThreadRemoved(thread); },
		[this](TextThread& thread, std::wstring& output) { return SentenceReceived(thread, output); }
	);
	current = &Host::GetThread(Host::console);
	Host::AddConsoleOutput(ABOUT);

	DWORD DUMMY;
	AttachConsole(ATTACH_PARENT_PROCESS);
	WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), CL_OPTIONS, wcslen(CL_OPTIONS), &DUMMY, NULL);
	std::vector<DWORD> processIds = Util::GetAllProcessIds();
	std::vector<std::wstring> processNames;
	for (auto processId : processIds) processNames.emplace_back(Util::GetModuleFilename(processId).value_or(L""));
	int argc;
	std::unique_ptr<LPWSTR[], Functor<LocalFree>> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
	for (int i = 0; i < argc; ++i)
		if (std::wstring arg = argv[i]; arg[0] == L'/' || arg[0] == L'-')
			if (arg[1] == L'p')
				if (DWORD processId = _wtoi(arg.substr(2).c_str())) Host::InjectProcess(processId);
				else for (int i = 0; i < processIds.size(); ++i)
					if (processNames[i].find(L"\\" + arg.substr(2)) != std::wstring::npos) Host::InjectProcess(processIds[i]);

	std::thread([]
	{
		using InternetHandle = AutoHandle<Functor<WinHttpCloseHandle>>;
		// Queries GitHub releases API https://developer.github.com/v3/repos/releases/ and checks the last release tag to check if it's the same
		if (InternetHandle internet = WinHttpOpen(L"Mozilla/5.0 Textractor", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0))
			if (InternetHandle connection = WinHttpConnect(internet, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0))
				if (InternetHandle request = WinHttpOpenRequest(connection, L"GET", L"/repos/Artikash/Textractor/releases", NULL, NULL, NULL, WINHTTP_FLAG_SECURE))
					if (WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, NULL))
					{
						DWORD bytesRead;
						char buffer[1000] = {};
						WinHttpReceiveResponse(request, NULL);
						WinHttpReadData(request, buffer, 1000, &bytesRead);
						if (abs(strstr(buffer, "/tag/") - strstr(buffer, CURRENT_VERSION)) > 10) Host::AddConsoleOutput(UPDATE_AVAILABLE);
					}
	}).detach();
}

MainWindow::~MainWindow()
{
	QSettings(CONFIG_FILE, QSettings::IniFormat).setValue(WINDOW, geometry());
	SetErrorMode(SEM_NOGPFAULTERRORBOX);
	ExitProcess(0);
}

void MainWindow::closeEvent(QCloseEvent*)
{
	QCoreApplication::quit(); // Need to do this to kill any windows that might've been made by extensions
}

bool MainWindow::isProcessSaved(const QString& process)
{
	bool result = false;

	QFile file(GAME_SAVE_FILE);
	if (file.open(QIODevice::ReadOnly))
	{
		QTextStream stream(&file);
		stream.setCodec("UTF-8");

		while (!stream.atEnd())
		{
			QString line = stream.readLine();
			if (process == line)
			{
				result = true;
				break;
			}
		}

		file.close();
	}

	return result;
}

void MainWindow::ProcessConnected(DWORD processId)
{
	if (processId == 0) return;
	QString process = S(Util::GetModuleFilename(processId).value_or(L"???"));
	QMetaObject::invokeMethod(this, [this, process, processId]
	{
		ui->processCombo->addItem(QString::number(processId, 16).toUpper() + ": " + QFileInfo(process).fileName());
	});
	if (process == "???") return;

	if (!isProcessSaved(process)) {
		QTextFile(GAME_SAVE_FILE, QIODevice::WriteOnly | QIODevice::Append).write((process + "\n").toUtf8());
	}

	QStringList allProcesses = QString(QTextFile(HOOK_SAVE_FILE, QIODevice::ReadOnly).readAll()).split("\n", QString::SkipEmptyParts);
	// Can't use QFileInfo::absoluteFilePath since hook save file has '\\' as path separator
	auto hookList = std::find_if(allProcesses.rbegin(), allProcesses.rend(), [&](QString hookList) { return hookList.contains(process); });
	if (hookList != allProcesses.rend())
		for (auto hookInfo : hookList->split(" , "))
			if (auto hp = Util::ParseCode(S(hookInfo))) Host::InsertHook(processId, hp.value());
			else swscanf_s(S(hookInfo).c_str(), L"|%I64d:%I64d:%[^\n]", &savedThreadCtx.first, &savedThreadCtx.second, savedThreadCode, (unsigned)std::size(savedThreadCode));
}

void MainWindow::ProcessDisconnected(DWORD processId)
{
	QMetaObject::invokeMethod(this, [this, processId]
	{
		ui->processCombo->removeItem(ui->processCombo->findText(QString::number(processId, 16).toUpper() + ":", Qt::MatchStartsWith));
	}, Qt::BlockingQueuedConnection);
}

void MainWindow::ThreadAdded(TextThread& thread)
{
	std::wstring threadCode = Util::GenerateCode(thread.hp, thread.tp.processId);
	QString ttString = TextThreadString(thread) + S(thread.name) + " (" + S(threadCode) + ")";
	bool savedMatch = savedThreadCtx.first == thread.tp.ctx && savedThreadCtx.second == thread.tp.ctx2 && savedThreadCode == threadCode;
	if (savedMatch) savedThreadCtx.first = savedThreadCtx.second = savedThreadCode[0] = 0;
	QMetaObject::invokeMethod(this, [this, ttString, savedMatch]
	{
		ui->ttCombo->addItem(ttString);
		if (savedMatch) ViewThread(ui->ttCombo->count() - 1);
	});
}

void MainWindow::ThreadRemoved(TextThread& thread)
{
	QString ttString = TextThreadString(thread);
	QMetaObject::invokeMethod(this, [this, ttString]
	{
		int threadIndex = ui->ttCombo->findText(ttString, Qt::MatchStartsWith);
		if (threadIndex == ui->ttCombo->currentIndex())	ViewThread(0);
		ui->ttCombo->removeItem(threadIndex);
	}, Qt::BlockingQueuedConnection);
}

bool MainWindow::SentenceReceived(TextThread& thread, std::wstring& sentence)
{
	if (!DispatchSentenceToExtensions(sentence, GetMiscInfo(thread).data())) return false;
	sentence += L'\n';
	if (current == &thread) QMetaObject::invokeMethod(this, [this, sentence]
	{
		ui->textOutput->moveCursor(QTextCursor::End);
		ui->textOutput->insertPlainText(S(sentence));
		ui->textOutput->moveCursor(QTextCursor::End);
	});
	return true;
}

QString MainWindow::TextThreadString(TextThread& thread)
{
	return QString("%1:%2:%3:%4:%5: ").arg(
		QString::number(thread.handle, 16),
		QString::number(thread.tp.processId, 16),
		QString::number(thread.tp.addr, 16),
		QString::number(thread.tp.ctx, 16),
		QString::number(thread.tp.ctx2, 16)
	).toUpper();
}

ThreadParam MainWindow::ParseTextThreadString(QString ttString)
{
	QStringList threadParam = ttString.split(":");
	return { threadParam[1].toUInt(nullptr, 16), threadParam[2].toULongLong(nullptr, 16), threadParam[3].toULongLong(nullptr, 16), threadParam[4].toULongLong(nullptr, 16) };
}

DWORD MainWindow::GetSelectedProcessId()
{
	return ui->processCombo->currentText().split(":")[0].toULong(nullptr, 16);
}

std::array<InfoForExtension, 10> MainWindow::GetMiscInfo(TextThread& thread)
{
	return
	{ {
	{ "current select", &thread == current },
	{ "text number", thread.handle },
	{ "process id", thread.tp.processId },
	{ "hook address", (int64_t)thread.tp.addr },
	{ "text handle", thread.handle },
	{ "text name", (int64_t)thread.name.c_str() },
	{ nullptr, 0 } // nullptr marks end of info array
	} };
}

void MainWindow::AttachProcess()
{
	QMultiHash<QString, DWORD> allProcesses;
	for (auto processId : Util::GetAllProcessIds())
		if (auto processName = Util::GetModuleFilename(processId)) allProcesses.insert(QFileInfo(S(processName.value())).fileName(), processId);

	QStringList processList(allProcesses.uniqueKeys());
	processList.sort(Qt::CaseInsensitive);
	if (QString process = QInputDialog::getItem(this, SELECT_PROCESS, ATTACH_INFO, processList, 0, true, &ok, Qt::WindowCloseButtonHint); ok)
		if (process.toInt(nullptr, 0)) Host::InjectProcess(process.toInt(nullptr, 0));
		else for (auto processId : allProcesses.values(process)) Host::InjectProcess(processId);
}

void MainWindow::LaunchProcess()
{
	QStringList savedProcesses = QString::fromUtf8(QTextFile(GAME_SAVE_FILE, QIODevice::ReadOnly).readAll()).split("\n", QString::SkipEmptyParts);
	std::reverse(savedProcesses.begin(), savedProcesses.end());
	int num = savedProcesses.removeDuplicates();

	if (num > 0)
	{
		//Clean-up duplicates from saved games
		auto file = QTextFile(GAME_SAVE_FILE, QIODevice::WriteOnly | QIODevice::Truncate);
		for (const auto& process : savedProcesses)
		{
			file.write(process.toUtf8());
			file.write(u8"\n");
		}
	}

	savedProcesses.push_back(SEARCH_GAME);
	std::wstring process = S(QInputDialog::getItem(this, SELECT_PROCESS, "", savedProcesses, 0, true, &ok, Qt::WindowCloseButtonHint));
	if (!ok) return;
	if (S(process) == SEARCH_GAME) process = S(QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, SELECT_PROCESS, "C:\\", PROCESSES)));
	if (process.find(L'\\') == std::wstring::npos) return;
	std::wstring path = std::wstring(process).erase(process.rfind(L'\\'));

	PROCESS_INFORMATION info = {};
	if (QMessageBox::question(this, SELECT_PROCESS, USE_JP_LOCALE) == QMessageBox::Yes)
		if (HMODULE localeEmulator = LoadLibraryOnce(L"LoaderDll"))
		{
			// see https://github.com/xupefei/Locale-Emulator/blob/aa99dec3b25708e676c90acf5fed9beaac319160/LEProc/LoaderWrapper.cs#L252
			struct
			{
				ULONG AnsiCodePage = SHIFT_JIS;
				ULONG OemCodePage = SHIFT_JIS;
				ULONG LocaleID = LANG_JAPANESE;
				ULONG DefaultCharset = SHIFTJIS_CHARSET;
				ULONG HookUiLanguageApi = FALSE;
				WCHAR DefaultFaceName[LF_FACESIZE] = {};
				TIME_ZONE_INFORMATION Timezone;
				ULONG64 Unused = 0;
			} LEB;
			GetTimeZoneInformation(&LEB.Timezone);
			((LONG(__stdcall*)(decltype(&LEB), LPCWSTR appName, LPWSTR commandLine, LPCWSTR currentDir, void*, void*, PROCESS_INFORMATION*, void*, void*, void*, void*))
				GetProcAddress(localeEmulator, "LeCreateProcess"))(&LEB, process.c_str(), NULL, path.c_str(), NULL, NULL, &info, NULL, NULL, NULL, NULL);
		}
	if (info.hProcess == NULL)
	{
		STARTUPINFOW DUMMY = { sizeof(DUMMY) };
		CreateProcessW(process.c_str(), NULL, nullptr, nullptr, FALSE, 0, nullptr, path.c_str(), &DUMMY, &info);
	}
	if (info.hProcess == NULL) return Host::AddConsoleOutput(LAUNCH_FAILED);
	Host::InjectProcess(info.dwProcessId);
	CloseHandle(info.hProcess);
	CloseHandle(info.hThread);
}

void MainWindow::DetachProcess()
{
	Host::DetachProcess(GetSelectedProcessId());
}

void MainWindow::AddHook()
{
	if (QString hookCode = QInputDialog::getText(this, ADD_HOOK, CODE_INFODUMP, QLineEdit::Normal, "", &ok, Qt::WindowCloseButtonHint); ok)
		if (auto hp = Util::ParseCode(S(hookCode))) Host::InsertHook(GetSelectedProcessId(), hp.value());
		else Host::AddConsoleOutput(INVALID_CODE);
}

void MainWindow::SaveHooks()
{
	if (auto processName = Util::GetModuleFilename(GetSelectedProcessId()))
	{
		QHash<uint64_t, QString> hookCodes;
		for (int i = 0; i < ui->ttCombo->count(); ++i)
		{
			ThreadParam tp = ParseTextThreadString(ui->ttCombo->itemText(i));
			if (tp.processId == GetSelectedProcessId())
			{
				HookParam hp = Host::GetHookParam(tp);
				if (!(hp.type & HOOK_ENGINE)) hookCodes[tp.addr] = S(Util::GenerateCode(hp, tp.processId));
			}
		}
		auto hookInfo = QStringList() << S(processName.value()) << hookCodes.values();
		TextThread& current = Host::GetThread(ParseTextThreadString(ui->ttCombo->currentText()));
		if (current.tp.processId == GetSelectedProcessId())
			hookInfo << QString("|%1:%2:%3").arg(current.tp.ctx).arg(current.tp.ctx2).arg(S(Util::GenerateCode(Host::GetHookParam(current.tp), current.tp.processId)));
		QTextFile(HOOK_SAVE_FILE, QIODevice::WriteOnly | QIODevice::Append).write((hookInfo.join(" , ") + "\n").toUtf8());
	}
}

void MainWindow::Settings()
{
	struct : QDialog
	{
		using QDialog::QDialog;
		void launch()
		{
			auto settings = new QSettings(CONFIG_FILE, QSettings::IniFormat, this);
			auto layout = new QFormLayout(this);
			auto save = new QPushButton(this);
			save->setText(SAVE_SETTINGS);
			layout->addWidget(save);
			for (auto[value, label] : Array<std::tuple<int&, const char*>>{
				{ Host::defaultCodepage, DEFAULT_CODEPAGE },
				{ TextThread::maxBufferSize, MAX_BUFFER_SIZE },
				{ TextThread::flushDelay, FLUSH_DELAY },
			})
			{
				auto spinBox = new QSpinBox(this);
				spinBox->setMaximum(INT_MAX);
				spinBox->setValue(value);
				layout->insertRow(0, label, spinBox);
				connect(save, &QPushButton::clicked, [=, &value] { settings->setValue(label, value = spinBox->value()); });
			}
			for (auto[value, label] : Array<std::tuple<bool&, const char*>>{
				{ TextThread::filterRepetition, FILTER_REPETITION },
			})
			{
				auto checkBox = new QCheckBox(this);
				checkBox->setChecked(value);
				layout->insertRow(0, label, checkBox);
				connect(save, &QPushButton::clicked, [=, &value] { settings->setValue(label, value = checkBox->isChecked()); });
			}
			connect(save, &QPushButton::clicked, this, &QDialog::accept);
			setWindowTitle(SETTINGS);
			exec();
		}
	} settingsDialog(this, Qt::WindowCloseButtonHint);
	settingsDialog.launch();
}

void MainWindow::Extensions()
{
	extenWindow->activateWindow();
	extenWindow->showNormal();
}

void MainWindow::ViewThread(int index)
{
	ui->ttCombo->setCurrentIndex(index);
	ui->textOutput->setPlainText(S((current = &Host::GetThread(ParseTextThreadString(ui->ttCombo->itemText(index))))->storage->c_str()));
	ui->textOutput->moveCursor(QTextCursor::End);
}
