# Overview

Universal Freight Platform enhances freight platforms by allowing them to **load and unload the same train car from a single platform**.

In vanilla Satisfactory, a freight platform must be configured either as **Load** or **Unload**. This mod removes that limitation by allowing both modes to be enabled simultaneously, making train logistics more flexible and reducing the number of platforms required in many setups.

The mod supports both:

* **Freight Platforms** (solid items)
* **Fluid Freight Platforms** (liquids and gases)

No changes are required to your existing railway network. Simply configure the platform modes according to your needs.

---

# How to Use

Open the configuration panel of any Freight Platform or Fluid Freight Platform.

Instead of a single Load/Unload selection, each mode now has its own toggle:

* **Load**: Enable or disable loading items or fluids into the train.
* **Unload**: Enable or disable unloading items or fluids from the train.

This allows three possible configurations:

| Load | Unload | Behavior                       |
| ---- | ------ | ------------------------------ |
| ON   | OFF    | Load only (vanilla behavior)   |
| OFF  | ON     | Unload only (vanilla behavior) |
| ON   | ON     | Bidirectional platform         |

### User Interface

![Freight UI](https://github.com/Loupimo/BidirectionalFreightPlatforms/blob/main/Resources/Freight_UI.png?raw=true)

![Fluid UI](https://github.com/Loupimo/BidirectionalFreightPlatforms/blob/main/Resources/Fluid_UI.png?raw=true)

---

# Good to Know

## Train operation order

When a train stops at a bidirectional platform (**Load = ON** and **Unload = ON**), operations are performed in the following order:

1. Unload cargo from the train
2. Load cargo into the train

This order is always respected.

## Existing platforms

Platforms that already exist in your save will keep their current configuration:

* Existing **Load** platforms remain Load only.
* Existing **Unload** platforms remain Unload only.

No automatic changes are made to existing factories.

## Newly built platforms

New Freight Platforms and Fluid Freight Platforms are created with both modes enabled by default:

* Load = ON
* Unload = ON

## Important

Any train stopping at a newly built or bidirectional platform will perform:

1. An unload operation
2. A load operation

unless train filters prevent one of these actions.

For this reason, it is recommended to review your train schedules and platform filters when introducing bidirectional platforms into an existing logistics network.

![Freight UI](https://github.com/Loupimo/BidirectionalFreightPlatforms/blob/main/Resources/Train_Demo.gif?raw=true)

# Support
For questions or support, contact:
- **Issue Tracker:** [GitHub Issues](https://github.com/Loupimo/BidirectionalFreightPlatforms/issues)

❤️ Support This Mod

This mod will always remain free and open source.

If you enjoy using it and would like to support future updates, bug fixes, and new projects, consider leaving a tip. Support is completely optional, but always appreciated.

☕ [Buy Loupimo a Coffee](https://ko-fi.com/loupimo)

Thank you for helping keep these projects alive!