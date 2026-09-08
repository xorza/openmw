#include "channel.hpp"

namespace Rtx
{
    std::string_view channelName(const Channel channel)
    {
        switch (channel)
        {
            case Channel::Direct:
                return "g-direct";
            case Channel::Indirect:
                return "g-indirect";
            case Channel::Albedo:
                return "g-albedo";
            case Channel::Specular:
                return "g-specular";
            case Channel::Guide:
                return "g-guide";
            case Channel::Motion:
                return "g-motion";
            case Channel::Depth:
                return "g-depth";
            case Channel::ReflectionMotion:
                return "g-reflection-motion";
            case Channel::ParticleMask:
                return "g-particle-mask";
            case Channel::BiasMask:
                return "g-bias-mask";
            case Channel::StarsShown:
                return "g-stars-shown";
            case Channel::Transparency:
                return "g-transparency";
            case Channel::TransparencyOpacity:
                return "g-transparency-opacity";
            case Channel::TransparencyMotion:
                return "g-transparency-motion";
        }

        return {};
    }
}
