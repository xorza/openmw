#include "screenshotwriter.hpp"

#include <functional>
#include <string>

#include <components/l10n/manager.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwgui/messagebox.hpp"

namespace MWRender
{
    namespace
    {
        struct ScreenCaptureMessageBox
        {
            void operator()(std::string filePath) const
            {
                if (filePath.empty())
                {
                    MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                        "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                    return;
                }

                auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
                std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    std::move(message), MWGui::ShowInDialogueMode_Never);
            }
        };

        struct IgnoreString
        {
            void operator()(std::string) const {}
        };
    }

    osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> makeScreenshotWriter(
        SceneUtil::WorkQueue& queue, const std::filesystem::path& screenshotPath)
    {
        return new SceneUtil::AsyncScreenCaptureOperation(&queue,
            new SceneUtil::WriteScreenshotToFileOperation(screenshotPath, Settings::general().mScreenshotFormat,
                Settings::general().mNotifyOnSavedScreenshot
                    ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                    : std::function<void(std::string)>(IgnoreString{})));
    }
}
