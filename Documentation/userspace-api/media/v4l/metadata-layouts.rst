.. SPDX-License-Identifier: GPL-2.0 OR GFDL-1.1-no-invariants-or-later

.. _media_metadata_layouts:

Metadata Layouts
----------------

The :ref:`metadata layout control <image_source_control_metadata_layout>`
specifies the on-bus layout of the metadata on pads with a :ref:`generic
metadata mbus code <media-bus-format-generic-meta>` independently of the bit
depth.

.. _media-metadata-layout-ccs:

MIPI CCS Embedded Data Layout (``V4L2_METADATA_LAYOUT_CCS``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

`MIPI CCS <https://www.mipi.org/specifications/camera-command-set>`_ defines a
metadata layout for sensor embedded data, identified by
``V4L2_CID_METADATA_LAYOUT`` control value ``V4L2_METADATA_LAYOUT_CCS``, which
is used to store the register configuration used for capturing a given
frame. The layout itself is defined in the CCS specification.

The CCS embedded data format (code ``0xa``) definition includes three levels:

1. Padding within CSI-2 bus :term:`Data Unit` as documented in the MIPI CCS
   specification.

2. The tagged data format as documented in the MIPI CCS specification.

3. Register addresses and register documentation as documented in the MIPI CCS
   specification.

The ``V4L2_METADATA_LAYOUT_CCS`` metadata layout value shall be used only by
devices that fulfill all three levels above.

This metadata layout code is only used for "2-byte simplified tagged data
format" (code ``0xa``) but their use may be extended further in the future, to
cover other CCS embedded data format codes.
