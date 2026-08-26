#include "postprocessingbindings.hpp"

#include "MyGUI_LanguageManager.h"

#include <components/lua/util.hpp>
#include <components/misc/strings/format.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwrender/gl/postprocessor.hpp"

#include "context.hpp"
#include "luamanagerimp.hpp"

namespace
{
    std::string getLocalizedMyGUIString(std::string_view unlocalized)
    {
        return MyGUI::LanguageManager::getInstance().replaceTags(std::string(unlocalized)).asUTF8();
    }
}

namespace MWLua
{
    struct Shader;
}

namespace sol
{
    template <>
    struct is_automagical<MWLua::Shader> : std::false_type
    {
    };
}

namespace MWLua
{
    struct Shader
    {
        std::shared_ptr<Fx::Technique> mShader;

        Shader(std::shared_ptr<Fx::Technique> shader)
            : mShader(std::move(shader))
        {
        }

        std::string toString() const
        {
            if (!mShader)
                return "Shader(nil)";

            return Misc::StringUtils::format("Shader(%s, %s)", mShader->getName(), mShader->getFileName().value());
        }

        enum
        {
            Action_None,
            Action_Enable,
            Action_Disable
        } mQueuedAction
            = Action_None;
    };

    namespace
    {
        /// The shader chain, or a Lua error naming why there is none.
        ///
        /// **Raised rather than ignored.** A script that sets a uniform on a renderer with no shader
        /// chain has asked for something that will not happen, and silently doing nothing would
        /// leave it looking for the mistake in its own arithmetic.
        MWRender::PostProcessor& postProcessor()
        {
            MWRender::PostProcessor* post = MWBase::Environment::get().getWorld()->getPostProcessor();
            if (post == nullptr)
                throw std::runtime_error("this renderer has no post-processing chain");
            return *post;
        }
    }

    template <class T>
    auto getSetter(const Context& context)
    {
        return [context](const Shader& shader, const std::string& name, const T& value) {
            context.mLuaManager->addAction(
                [=] { postProcessor().setUniform(shader.mShader, name, value); }, "SetUniformShaderAction");
        };
    }

    template <class T>
    auto getArraySetter(const Context& context)
    {
        return [context](const Shader& shader, const std::string& name, const sol::table& table) {
            auto targetSize = postProcessor().getUniformSize(shader.mShader, name);

            if (!targetSize.has_value())
                throw std::runtime_error(Misc::StringUtils::format("Failed setting uniform array '%s'", name));

            if (*targetSize != table.size())
                throw std::runtime_error(Misc::StringUtils::format(
                    "Mismatching uniform array size, got %zu expected %zu", table.size(), *targetSize));

            std::vector<T> values;
            values.reserve(*targetSize);

            for (size_t i = 0; i < *targetSize; ++i)
            {
                sol::object obj = table[LuaUtil::toLuaIndex(i)];
                if (!obj.is<T>())
                    throw std::runtime_error("Invalid type for uniform array");
                values.push_back(obj.as<T>());
            }

            context.mLuaManager->addAction(
                [shader, name, values = std::move(values)] {
                    postProcessor().setUniform(shader.mShader, name, values);
                },
                "SetUniformShaderAction");
        };
    }

    sol::table initPostprocessingPackage(const Context& context)
    {
        sol::state_view lua = context.sol();
        sol::table api(lua, sol::create);

        {
            sol::usertype<Shader> shader = lua.new_usertype<Shader>("Shader");
            shader[sol::meta_function::to_string] = [](const Shader& self) { return self.toString(); };

            shader["enable"] = [context](Shader& self, sol::optional<int> optPos) {
                std::optional<int> pos = std::nullopt;
                if (optPos)
                    pos = optPos.value();

                if (self.mShader && self.mShader->isValid())
                    self.mQueuedAction = Shader::Action_Enable;

                context.mLuaManager->addAction([=, &self] {
                    self.mQueuedAction = Shader::Action_None;

                    if (postProcessor().enableTechnique(self.mShader, pos) == MWRender::PostProcessor::Status_Error)
                        throw std::runtime_error("Failed enabling shader '" + self.mShader->getName() + "'");
                });
            };

            shader["disable"] = [context](Shader& self) {
                self.mQueuedAction = Shader::Action_Disable;

                context.mLuaManager->addAction([&] {
                    self.mQueuedAction = Shader::Action_None;

                    if (postProcessor().disableTechnique(self.mShader) == MWRender::PostProcessor::Status_Error)
                        throw std::runtime_error("Failed disabling shader '" + self.mShader->getName() + "'");
                });
            };

            shader["isEnabled"] = [](const Shader& self) {
                if (self.mQueuedAction == Shader::Action_Enable)
                    return true;
                else if (self.mQueuedAction == Shader::Action_Disable)
                    return false;
                return postProcessor().isTechniqueEnabled(self.mShader);
            };

            shader["name"] = sol::readonly_property(
                [](const Shader& self) { return getLocalizedMyGUIString(self.mShader->getName()); });
            shader["author"] = sol::readonly_property(
                [](const Shader& self) { return getLocalizedMyGUIString(self.mShader->getAuthor()); });
            shader["description"] = sol::readonly_property(
                [](const Shader& self) { return getLocalizedMyGUIString(self.mShader->getDescription()); });
            shader["version"] = sol::readonly_property(
                [](const Shader& self) { return getLocalizedMyGUIString(self.mShader->getVersion()); });

            shader["setBool"] = getSetter<bool>(context);
            shader["setFloat"] = getSetter<float>(context);
            shader["setInt"] = getSetter<int>(context);
            shader["setVector2"] = getSetter<osg::Vec2f>(context);
            shader["setVector3"] = getSetter<osg::Vec3f>(context);
            shader["setVector4"] = getSetter<osg::Vec4f>(context);

            shader["setFloatArray"] = getArraySetter<float>(context);
            shader["setIntArray"] = getArraySetter<int>(context);
            shader["setVector2Array"] = getArraySetter<osg::Vec2f>(context);
            shader["setVector3Array"] = getArraySetter<osg::Vec3f>(context);
            shader["setVector4Array"] = getArraySetter<osg::Vec4f>(context);
        }

        api["load"] = [](const std::string& name) {
            Shader shader{ postProcessor().loadTechnique(name, false) };

            if (!shader.mShader || !shader.mShader->isValid())
                throw std::runtime_error(Misc::StringUtils::format("Failed loading shader '%s'", name));

            return shader;
        };

        api["getChain"] = [context]() {
            sol::table chain(context.sol(), sol::create);

            for (const auto& shader : postProcessor().getChain())
            {
                // Don't expose internal shaders to the API, they should be invisible to the user
                if (shader->getInternal())
                    continue;
                chain.add(Shader(shader));
            }

            return chain;
        };

        return LuaUtil::makeReadOnly(api);
    }

}
