#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtx/shaders/visibility.h>

namespace Rtx
{
    namespace
    {
        /// The hit table says, at every record, the eye and the layer the launch reaches that
        /// record for: three kinds, each standing behind two eyes' runs of five layers.
        ///
        /// **Hand-placed: thirty records, and the water's arms' fourth layer is the twenty-ninth.**
        /// `2 * 10 + 1 * 5 + 3` is 28, which the instance's offset for water, `hitRecordOffset`
        /// for the arms and the launch's layer reach between them.
        TEST(RtxHitRecordTest, everyRecordSaysTheEyeAndTheLayerTheLaunchReachesItFor)
        {
            const auto table = Shaders::hitRecordTable();
            EXPECT_EQ(table.size(), 30u);
            EXPECT_EQ(Shaders::HIT_RECORDS_PER_SHADER, 10u);

            const Shaders::HitRecord& waterArmsLayerThree = table[28];
            EXPECT_EQ(waterArmsLayerThree.mArms, 1u);
            EXPECT_EQ(waterArmsLayerThree.mLayer, 3u);

            for (std::uint32_t kind = 0; kind < Shaders::HIT_SHADER_COUNT; ++kind)
                for (std::uint32_t arms = 0; arms < Shaders::HIT_RECORD_EYES; ++arms)
                    for (std::uint32_t layer = 0; layer < Shaders::HIT_RECORD_LAYERS; ++layer)
                    {
                        const Shaders::HitRecord& record
                            = table[kind * Shaders::HIT_RECORDS_PER_SHADER + Shaders::hitRecordOffset(arms, layer)];
                        EXPECT_EQ(record.mArms, arms) << "kind " << kind << " layer " << layer;
                        EXPECT_EQ(record.mLayer, layer) << "kind " << kind << " arms " << arms;
                    }
        }
    }
}
