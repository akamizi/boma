#include "core/ProcessRunner.h"

#include <QProcess>
#include <QTimer>
#include <memory>

namespace boma {

QProcess* ProcessRunner::run(const QString& program, const QStringList& args,
                        std::chrono::milliseconds timeout,
                        std::function<void(ProcessResult)> done,
                        OutputCallback onOutput)
{
    auto* proc = new QProcess(this);
    auto result = std::make_shared<ProcessResult>();
    auto finished = std::make_shared<bool>(false);

    auto finish = [proc, result, finished, done = std::move(done)]() {
        if (*finished)
            return;
        *finished = true;
        result->out += proc->readAllStandardOutput();
        result->err += proc->readAllStandardError();
        proc->deleteLater();
        if (done)
            done(*result);
    };

    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, result, onOutput]() {
        const QByteArray d = proc->readAllStandardOutput();
        result->out += d;
        if (onOutput)
            onOutput(d);
    });
    connect(proc, &QProcess::readyReadStandardError, this, [proc, result, onOutput]() {
        const QByteArray d = proc->readAllStandardError();
        result->err += d;
        if (onOutput)
            onOutput(d);
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, result, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            result->exitCode = -1;
            result->err += proc->errorString().toUtf8();
            finish();
        }
    });
    connect(proc, &QProcess::finished, this,
            [result, finish](int code, QProcess::ExitStatus st) {
                result->exitCode = code;
                result->crashed = (st == QProcess::CrashExit) && !result->timedOut;
                finish();
            });

    if (timeout.count() > 0) {
        auto* t = new QTimer(proc);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, proc, [proc, result]() {
            if (proc->state() != QProcess::NotRunning) {
                result->timedOut = true;
                proc->kill();
            }
        });
        t->start(static_cast<int>(timeout.count()));
    }

    proc->start(program, args);
    return proc;
}

} // namespace boma
