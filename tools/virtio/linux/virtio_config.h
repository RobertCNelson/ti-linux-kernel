#include <linux/virtio_byteorder.h>
#include <linux/virtio.h>

#define VIRTIO_TRANSPORT_F_START	28
#define VIRTIO_TRANSPORT_F_END		32
/*
 * __virtio_test_bit - helper to test feature bits. For use by transports.
 *                     Devices should normally use virtio_has_feature,
 *                     which includes more checks.
 * @vdev: the device
 * @fbit: the feature bit
 */
static inline bool __virtio_test_bit(const struct virtio_device *vdev,
				     unsigned int fbit)
{
	return vdev->features & (1ULL << fbit);
}

#define virtio_has_feature(dev, feature) \
	(__virtio_test_bit((dev), feature))

static inline u16 virtio16_to_cpu(struct virtio_device *vdev, __virtio16 val)
{
	return __virtio16_to_cpu(virtio_has_feature(vdev, VIRTIO_F_VERSION_1), val);
}

static inline __virtio16 cpu_to_virtio16(struct virtio_device *vdev, u16 val)
{
	return __cpu_to_virtio16(virtio_has_feature(vdev, VIRTIO_F_VERSION_1), val);
}

static inline u32 virtio32_to_cpu(struct virtio_device *vdev, __virtio32 val)
{
	return __virtio32_to_cpu(virtio_has_feature(vdev, VIRTIO_F_VERSION_1), val);
}

static inline __virtio32 cpu_to_virtio32(struct virtio_device *vdev, u32 val)
{
	return __cpu_to_virtio32(virtio_has_feature(vdev, VIRTIO_F_VERSION_1), val);
}

static inline u64 virtio64_to_cpu(struct virtio_device *vdev, __virtio64 val)
{
	return __virtio64_to_cpu(virtio_has_feature(vdev, VIRTIO_F_VERSION_1), val);
}

static inline __virtio64 cpu_to_virtio64(struct virtio_device *vdev, u64 val)
{
	return __cpu_to_virtio64(virtio_has_feature(vdev, VIRTIO_F_VERSION_1), val);
}

