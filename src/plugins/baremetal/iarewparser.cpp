/****************************************************************************
**
** Copyright (C) 2019 Denis Shienkov <denis.shienkov@gmail.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "iarewparser.h"

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/task.h>

#include <texteditor/fontsettings.h>
#include <texteditor/texteditorsettings.h>

#include <QRegularExpression>

using namespace ProjectExplorer;

namespace BareMetal {
namespace Internal {

static Task::TaskType taskType(const QString &msgType)
{
    if (msgType == "Warning")
        return Task::TaskType::Warning;
    else if (msgType == "Error" || msgType == "Fatal error")
        return Task::TaskType::Error;
    return Task::TaskType::Unknown;
}

IarParser::IarParser()
{
    setObjectName("IarParser");
}

Core::Id IarParser::id()
{
    return "BareMetal.OutputParser.Iar";
}

void IarParser::newTask(const Task &task)
{
    doFlush();
    m_lastTask = task;
    m_lines = 1;
}

void IarParser::amendDescription()
{
    while (!m_descriptionParts.isEmpty())
        m_lastTask.description.append(m_descriptionParts.takeFirst());

    while (!m_snippets.isEmpty()) {
        const QString snippet = m_snippets.takeFirst();
        const int start = m_lastTask.description.count() + 1;
        m_lastTask.description.append(QLatin1Char('\n'));
        m_lastTask.description.append(snippet);

        QTextLayout::FormatRange fr;
        fr.start = start;
        fr.length = m_lastTask.description.count() + 1;
        fr.format.setFont(TextEditor::TextEditorSettings::fontSettings().font());
        fr.format.setFontStyleHint(QFont::Monospace);
        m_lastTask.formats.append(fr);

        ++m_lines;
    }
}

void IarParser::amendFilePath()
{
    if (m_filePathParts.isEmpty())
        return;
    QString filePath;
    while (!m_filePathParts.isEmpty())
        filePath.append(m_filePathParts.takeFirst().trimmed());
    m_lastTask.file = Utils::FileName::fromUserInput(filePath);
    m_expectFilePath = false;
}

void IarParser::stdError(const QString &line)
{
    IOutputParser::stdError(line);

    const QString lne = rightTrimmed(line);

    QRegularExpression re;
    QRegularExpressionMatch match;

    re.setPattern("^(Error|Fatal error)\\[(.+)\\]:\\s(.+)\\s\\[(.+)$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { MessageTypeIndex = 1, MessageCodeIndex,
                            DescriptionIndex, FilepathBeginIndex };
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        const QString descr = QString("[%1]: %2").arg(match.captured(MessageCodeIndex),
                                                      match.captured(DescriptionIndex));
        // This task has a file path, but this patch are split on
        // some lines, which will be received later.
        const Task task(type, descr, {}, -1, Constants::TASK_CATEGORY_COMPILE);
        newTask(task);
        // Prepare first part of a file path.
        QString firstPart = match.captured(FilepathBeginIndex);
        firstPart.remove("referenced from ");
        m_filePathParts.push_back(firstPart);
        m_expectFilePath = true;
        m_expectSnippet = false;
        return;
    }

    re.setPattern("^.*(Error|Fatal error)\\[(.+)\\]:\\s(.+)$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { MessageTypeIndex = 1, MessageCodeIndex,
                            DescriptionIndex };
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        const QString descr = QString("[%1]: %2").arg(match.captured(MessageCodeIndex),
                                                      match.captured(DescriptionIndex));
        // This task has not a file path. The description details
        // will be received later on the next lines.
        const Task task(type, descr, {}, -1, Constants::TASK_CATEGORY_COMPILE);
        newTask(task);
        m_expectSnippet = true;
        m_expectFilePath = false;
        m_expectDescription = false;
        return;
    }

    re.setPattern("^\"(.+)\",(\\d+)?\\s+(Warning|Error|Fatal error)\\[(.+)\\].+$");
    match = re.match(lne);
    if (match.hasMatch()) {
        enum CaptureIndex { FilePathIndex = 1, LineNumberIndex,
                            MessageTypeIndex, MessageCodeIndex };
        const Utils::FileName fileName = Utils::FileName::fromUserInput(
                    match.captured(FilePathIndex));
        const int lineno = match.captured(LineNumberIndex).toInt();
        const Task::TaskType type = taskType(match.captured(MessageTypeIndex));
        // A full description will be received later on next lines.
        const Task task(type, {}, fileName, lineno, Constants::TASK_CATEGORY_COMPILE);
        newTask(task);
        const QString firstPart = QString("[%1]: ").arg(match.captured(MessageCodeIndex));
        m_descriptionParts.append(firstPart);
        m_expectDescription = true;
        m_expectSnippet = false;
        m_expectFilePath = false;
        return;
    }

    if (lne.isEmpty()) {
        //
    } else if (!lne.startsWith(QLatin1Char(' '))) {
        return;
    } else if (m_expectFilePath) {
        if (lne.endsWith(QLatin1Char(']'))) {
            const QString lastPart = lne.left(lne.size() - 1);
            m_filePathParts.push_back(lastPart);
        } else {
            m_filePathParts.push_back(lne);
            return;
        }
    } else if (m_expectSnippet) {
        if (!lne.endsWith("Fatal error detected, aborting.")) {
            m_snippets.push_back(lne);
            return;
        }
    } else if (m_expectDescription) {
        if (!lne.startsWith("            ")) {
            m_descriptionParts.push_back(lne.trimmed());
            return;
        }
    }

    doFlush();
}

void IarParser::stdOutput(const QString &line)
{
    IOutputParser::stdOutput(line);

    const QString lne = rightTrimmed(line);
    if (!lne.startsWith("Error in command line"))
        return;

    const Task task(Task::TaskType::Error, line.trimmed(), {},
                    -1, Constants::TASK_CATEGORY_COMPILE);
    newTask(task);
    doFlush();
}

void IarParser::doFlush()
{
    if (m_lastTask.isNull())
        return;

    amendDescription();
    amendFilePath();

    m_expectSnippet = true;
    m_expectFilePath = false;
    m_expectDescription = false;

    Task t = m_lastTask;
    m_lastTask.clear();
    emit addTask(t, m_lines, 1);
    m_lines = 0;
}

} // namespace Internal
} // namespace BareMetal

// Unit tests:

#ifdef WITH_TESTS
#include "baremetalplugin.h"
#include <projectexplorer/outputparser_test.h>
#include <QTest>

namespace BareMetal {
namespace Internal {

void BareMetalPlugin::testIarOutputParsers_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<OutputParserTester::Channel>("inputChannel");
    QTest::addColumn<QString>("childStdOutLines");
    QTest::addColumn<QString>("childStdErrLines");
    QTest::addColumn<QList<ProjectExplorer::Task> >("tasks");
    QTest::addColumn<QString>("outputLines");

    QTest::newRow("pass-through stdout")
            << "Sometext" << OutputParserTester::STDOUT
            << "Sometext\n" << QString()
            << QList<Task>()
            << QString();
    QTest::newRow("pass-through stderr")
            << "Sometext" << OutputParserTester::STDERR
            << QString() << "Sometext\n"
            << QList<Task>()
            << QString();

    const Core::Id categoryCompile = Constants::TASK_CATEGORY_COMPILE;

    // For std out.
    QTest::newRow("Error in command line")
            << QString::fromLatin1("Error in command line: Some error")
            << OutputParserTester::STDOUT
            << QString::fromLatin1("Error in command line: Some error\n")
            << QString()
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("Error in command line: Some error"),
                                      Utils::FileName(),
                                      -1,
                                      categoryCompile))
            << QString();

    // For std error.
    QTest::newRow("No details warning")
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Warning[Pe223]:\n"
                                   "          Some warning \"foo\" bar")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Warning[Pe223]:\n"
                                   "          Some warning \"foo\" bar\n")
            << (QList<Task>() << Task(Task::Warning,
                                      QLatin1String("[Pe223]: Some warning \"foo\" bar"),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\main.c")),
                                      63,
                                      categoryCompile))
            << QString();

    QTest::newRow("Details warning")
            << QString::fromLatin1("      some_detail;\n"
                                   "      ^\n"
                                   "\"c:\\foo\\main.c\",63 Warning[Pe223]:\n"
                                   "          Some warning")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("      some_detail;\n"
                                   "      ^\n"
                                   "\"c:\\foo\\main.c\",63 Warning[Pe223]:\n"
                                   "          Some warning\n")
            << (QList<Task>() << Task(Task::Warning,
                                      QLatin1String("[Pe223]: Some warning\n"
                                                    "      some_detail;\n"
                                                    "      ^"),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\main.c")),
                                      63,
                                      categoryCompile))
            << QString();

    QTest::newRow("No details split-description warning")
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Warning[Pe223]:\n"
                                   "          Some warning\n"
                                   "          , split")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Warning[Pe223]:\n"
                                   "          Some warning\n"
                                   "          , split\n")
            << (QList<Task>() << Task(Task::Warning,
                                      QLatin1String("[Pe223]: Some warning, split"),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\main.c")),
                                      63,
                                      categoryCompile))
            << QString();

    QTest::newRow("No details error")
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Error[Pe223]:\n"
                                   "          Some error")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Error[Pe223]:\n"
                                   "          Some error\n")
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("[Pe223]: Some error"),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\main.c")),
                                      63,
                                      categoryCompile))
            << QString();

    QTest::newRow("Details error")
            << QString::fromLatin1("      some_detail;\n"
                                   "      ^\n"
                                   "\"c:\\foo\\main.c\",63 Error[Pe223]:\n"
                                   "          Some error")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("      some_detail;\n"
                                   "      ^\n"
                                   "\"c:\\foo\\main.c\",63 Error[Pe223]:\n"
                                   "          Some error\n")
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("[Pe223]: Some error\n"
                                                    "      some_detail;\n"
                                                    "      ^"),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\main.c")),
                                      63,
                                      categoryCompile))
            << QString();

    QTest::newRow("No details split-description error")
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Error[Pe223]:\n"
                                   "          Some error\n"
                                   "          , split")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("\"c:\\foo\\main.c\",63 Error[Pe223]:\n"
                                   "          Some error\n"
                                   "          , split\n")
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("[Pe223]: Some error, split"),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\main.c")),
                                      63,
                                      categoryCompile))
            << QString();

    QTest::newRow("No definition for")
            << QString::fromLatin1("Error[Li005]: Some error \"foo\" [referenced from c:\\fo\n"
                                   "         o\\bar\\mai\n"
                                   "         n.c.o\n"
                                   "]")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("Error[Li005]: Some error \"foo\" [referenced from c:\\fo\n"
                                   "         o\\bar\\mai\n"
                                   "         n.c.o\n"
                                   "]\n")
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("[Li005]: Some error \"foo\""),
                                      Utils::FileName::fromUserInput(QLatin1String("c:\\foo\\bar\\main.c.o")),
                                      -1,
                                      categoryCompile))
            << QString();

    QTest::newRow("More than one source file specified")
            << QString::fromLatin1("Fatal error[Su011]: Some error:\n"
                                   "                      c:\\foo.c\n"
                                   "            c:\\bar.c\n"
                                   "Fatal error detected, aborting.")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("Fatal error[Su011]: Some error:\n"
                                   "                      c:\\foo.c\n"
                                   "            c:\\bar.c\n"
                                   "Fatal error detected, aborting.\n")
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("[Su011]: Some error:\n"
                                                    "                      c:\\foo.c\n"
                                                    "            c:\\bar.c"),
                                      Utils::FileName(),
                                      -1,
                                      categoryCompile))
            << QString();

    QTest::newRow("At end of source")
            << QString::fromLatin1("At end of source  Error[Pe040]: Some error \";\"")
            << OutputParserTester::STDERR
            << QString()
            << QString::fromLatin1("At end of source  Error[Pe040]: Some error \";\"\n")
            << (QList<Task>() << Task(Task::Error,
                                      QLatin1String("[Pe040]: Some error \";\""),
                                      Utils::FileName(),
                                      -1,
                                      categoryCompile))
            << QString();
}

void BareMetalPlugin::testIarOutputParsers()
{
    OutputParserTester testbench;
    testbench.appendOutputParser(new IarParser);
    QFETCH(QString, input);
    QFETCH(OutputParserTester::Channel, inputChannel);
    QFETCH(QList<Task>, tasks);
    QFETCH(QString, childStdOutLines);
    QFETCH(QString, childStdErrLines);
    QFETCH(QString, outputLines);

    testbench.testParsing(input, inputChannel,
                          tasks, childStdOutLines, childStdErrLines,
                          outputLines);
}

} // namespace Internal
} // namespace BareMetal

#endif // WITH_TESTS
