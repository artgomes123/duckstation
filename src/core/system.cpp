#include "system.h"
#include "YBaseLib/AutoReleasePtr.h"
#include "YBaseLib/Log.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "dma.h"
#include "gpu.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "memory_card.h"
#include "pad.h"
#include "pad_device.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
#include <imgui.h>
Log_SetChannel(System);

System::System(HostInterface* host_interface) : m_host_interface(host_interface)
{
  m_cpu = std::make_unique<CPU::Core>();
  m_bus = std::make_unique<Bus>();
  m_dma = std::make_unique<DMA>();
  m_interrupt_controller = std::make_unique<InterruptController>();
  m_cdrom = std::make_unique<CDROM>();
  m_pad = std::make_unique<Pad>();
  m_timers = std::make_unique<Timers>();
  m_spu = std::make_unique<SPU>();
  m_mdec = std::make_unique<MDEC>();
  m_region = host_interface->GetSettings().region;
}

System::~System() = default;

std::optional<ConsoleRegion> System::GetRegionForCDImage(const CDImage* image)
{
  // TODO: Implement me.
  return ConsoleRegion::NTSC_U;
}

bool System::IsPSExe(const char* filename)
{
  const StaticString filename_str(filename);
  return filename_str.EndsWith(".psexe", false) || filename_str.EndsWith(".exe", false);
}

std::unique_ptr<System> System::Create(HostInterface* host_interface)
{
  std::unique_ptr<System> system(new System(host_interface));
  if (!system->CreateGPU())
    return {};

  return system;
}

bool System::RecreateGPU()
{
  // save current state
  AutoReleasePtr<ByteStream> state_stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper sw(state_stream, StateWrapper::Mode::Write);
  const bool state_valid = m_gpu->DoState(sw);
  if (!state_valid)
    Log_ErrorPrintf("Failed to save old GPU state when switching renderers");

  // create new renderer
  m_gpu.reset();
  if (!CreateGPU())
  {
    Panic("Failed to recreate GPU");
    return false;
  }

  if (state_valid)
  {
    state_stream->SeekAbsolute(0);
    sw.SetMode(StateWrapper::Mode::Read);
    m_gpu->DoState(sw);
  }

  return true;
}

bool System::Boot(const char* filename)
{
  // Load CD image up and detect region.
  std::unique_ptr<CDImage> media;
  bool exe_boot = false;
  if (filename)
  {
    exe_boot = IsPSExe(filename);
    if (exe_boot)
    {
      if (m_region == ConsoleRegion::Auto)
      {
        Log_InfoPrintf("Defaulting to NTSC-U region for executable.");
        m_region = ConsoleRegion::NTSC_U;
      }
    }
    else
    {
      Log_InfoPrintf("Loading CD image '%s'...", filename);
      media = CDImage::Open(filename);
      if (!media)
      {
        m_host_interface->ReportError(SmallString::FromFormat("Failed to load CD image '%s'", filename));
        return false;
      }

      if (m_region == ConsoleRegion::Auto)
      {
        std::optional<ConsoleRegion> detected_region = GetRegionForCDImage(media.get());
        m_region = detected_region.value_or(ConsoleRegion::NTSC_U);
        if (detected_region)
          Log_InfoPrintf("Auto-detected %s region for '%s'", Settings::GetConsoleRegionName(m_region));
        else
          Log_WarningPrintf("Could not determine region for CD. Defaulting to NTSC-U.");
      }
    }
  }
  else
  {
    // Default to NTSC for BIOS boot.
    if (m_region == ConsoleRegion::Auto)
      m_region = ConsoleRegion::NTSC_U;
  }

  // Load BIOS image.
  std::optional<BIOS::Image> bios_image = m_host_interface->GetBIOSImage(m_region);
  if (!bios_image)
  {
    m_host_interface->ReportError(
      TinyString::FromFormat("Failed to load %s BIOS", Settings::GetConsoleRegionName(m_region)));
    return false;
  }

  // Component setup.
  InitializeComponents();
  UpdateMemoryCards();

  // Enable tty by patching bios.
  const BIOS::Hash bios_hash = BIOS::GetHash(*bios_image);
  if (GetSettings().bios_patch_tty_enable)
    BIOS::PatchBIOSEnableTTY(*bios_image, bios_hash);

  // Load EXE late after BIOS.
  if (exe_boot && !LoadEXE(filename, *bios_image))
  {
    m_host_interface->ReportError(SmallString::FromFormat("Failed to load EXE file '%s'", filename));
    return false;
  }

  // Insert CD, and apply fastboot patch if enabled.
  m_cdrom->InsertMedia(std::move(media));
  if (m_cdrom->HasMedia() && GetSettings().bios_patch_fast_boot)
    BIOS::PatchBIOSFastBoot(*bios_image, bios_hash);

  // Load the patched BIOS up.
  m_bus->SetBIOS(*bios_image);

  // Good to go.
  Reset();
  return true;
}

void System::InitializeComponents()
{
  m_cpu->Initialize(m_bus.get());
  m_bus->Initialize(m_cpu.get(), m_dma.get(), m_interrupt_controller.get(), m_gpu.get(), m_cdrom.get(), m_pad.get(),
                    m_timers.get(), m_spu.get(), m_mdec.get());

  m_dma->Initialize(this, m_bus.get(), m_interrupt_controller.get(), m_gpu.get(), m_cdrom.get(), m_spu.get(),
                    m_mdec.get());

  m_interrupt_controller->Initialize(m_cpu.get());

  m_cdrom->Initialize(this, m_dma.get(), m_interrupt_controller.get(), m_spu.get());
  m_pad->Initialize(this, m_interrupt_controller.get());
  m_timers->Initialize(this, m_interrupt_controller.get());
  m_spu->Initialize(this, m_dma.get(), m_interrupt_controller.get());
  m_mdec->Initialize(this, m_dma.get());
}

bool System::CreateGPU()
{
  switch (m_host_interface->GetSettings().gpu_renderer)
  {
    case GPURenderer::HardwareOpenGL:
      m_gpu = GPU::CreateHardwareOpenGLRenderer();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      m_gpu = GPU::CreateHardwareD3D11Renderer();
      break;
#endif

    case GPURenderer::Software:
    default:
      m_gpu = GPU::CreateSoftwareRenderer();
      break;
  }

  if (!m_gpu || !m_gpu->Initialize(m_host_interface->GetDisplay(), this, m_dma.get(), m_interrupt_controller.get(),
                                   m_timers.get()))
  {
    Log_ErrorPrintf("Failed to initialize GPU, falling back to software");
    m_gpu.reset();
    m_host_interface->GetSettings().gpu_renderer = GPURenderer::Software;
    m_gpu = GPU::CreateSoftwareRenderer();
    if (!m_gpu->Initialize(m_host_interface->GetDisplay(), this, m_dma.get(), m_interrupt_controller.get(),
                           m_timers.get()))
    {
      return false;
    }
  }

  m_bus->SetGPU(m_gpu.get());
  m_dma->SetGPU(m_gpu.get());
  return true;
}

bool System::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("System"))
    return false;

  sw.Do(&m_frame_number);
  sw.Do(&m_internal_frame_number);
  sw.Do(&m_global_tick_counter);

  if (!sw.DoMarker("CPU") || !m_cpu->DoState(sw))
    return false;

  if (!sw.DoMarker("Bus") || !m_bus->DoState(sw))
    return false;

  if (!sw.DoMarker("DMA") || !m_dma->DoState(sw))
    return false;

  if (!sw.DoMarker("InterruptController") || !m_interrupt_controller->DoState(sw))
    return false;

  if (!sw.DoMarker("GPU") || !m_gpu->DoState(sw))
    return false;

  if (!sw.DoMarker("CDROM") || !m_cdrom->DoState(sw))
    return false;

  if (!sw.DoMarker("Pad") || !m_pad->DoState(sw))
    return false;

  if (!sw.DoMarker("Timers") || !m_timers->DoState(sw))
    return false;

  if (!sw.DoMarker("SPU") || !m_spu->DoState(sw))
    return false;

  if (!sw.DoMarker("MDEC") || !m_mdec->DoState(sw))
    return false;

  return !sw.HasError();
}

void System::Reset()
{
  m_cpu->Reset();
  m_bus->Reset();
  m_dma->Reset();
  m_interrupt_controller->Reset();
  m_gpu->Reset();
  m_cdrom->Reset();
  m_pad->Reset();
  m_timers->Reset();
  m_spu->Reset();
  m_mdec->Reset();
  m_frame_number = 1;
  m_internal_frame_number = 0;
  m_global_tick_counter = 0;
}

bool System::LoadState(ByteStream* state)
{
  StateWrapper sw(state, StateWrapper::Mode::Read);
  return DoState(sw);
}

bool System::SaveState(ByteStream* state)
{
  StateWrapper sw(state, StateWrapper::Mode::Write);
  return DoState(sw);
}

void System::RunFrame()
{
  u32 current_frame_number = m_frame_number;
  while (current_frame_number == m_frame_number)
  {
    m_cpu->Execute();
    Synchronize();
  }
}

bool System::LoadEXE(const char* filename, std::vector<u8>& bios_image)
{
#pragma pack(push, 1)
  struct EXEHeader
  {
    char id[8];            // 0x000-0x007 PS-X EXE
    char pad1[8];          // 0x008-0x00F
    u32 initial_pc;        // 0x010
    u32 initial_gp;        // 0x014
    u32 load_address;      // 0x018
    u32 file_size;         // 0x01C excluding 0x800-byte header
    u32 unk0;              // 0x020
    u32 unk1;              // 0x024
    u32 memfill_start;     // 0x028
    u32 memfill_size;      // 0x02C
    u32 initial_sp_base;   // 0x030
    u32 initial_sp_offset; // 0x034
    u32 reserved[5];       // 0x038-0x04B
    char marker[0x7B4];    // 0x04C-0x7FF
  };
  static_assert(sizeof(EXEHeader) == 0x800);
#pragma pack(pop)

  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
    return false;

  EXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1)
  {
    std::fclose(fp);
    return false;
  }

  if (header.memfill_size > 0)
  {
    const u32 words_to_write = header.memfill_size / 4;
    u32 address = header.memfill_start & ~UINT32_C(3);
    for (u32 i = 0; i < words_to_write; i++)
    {
      m_cpu->SafeWriteMemoryWord(address, 0);
      address += sizeof(u32);
    }
  }

  if (header.file_size >= 4)
  {
    std::vector<u32> data_words(header.file_size / 4);
    if (std::fread(data_words.data(), header.file_size, 1, fp) != 1)
    {
      std::fclose(fp);
      return false;
    }

    const u32 num_words = header.file_size / 4;
    u32 address = header.load_address;
    for (u32 i = 0; i < num_words; i++)
    {
      m_cpu->SafeWriteMemoryWord(address, data_words[i]);
      address += sizeof(u32);
    }
  }

  std::fclose(fp);

  // patch the BIOS to jump to the executable directly
  const u32 r_pc = header.load_address;
  const u32 r_gp = header.initial_gp;
  const u32 r_sp = header.initial_sp_base;
  const u32 r_fp = header.initial_sp_base + header.initial_sp_offset;
  return BIOS::PatchBIOSForEXE(bios_image, r_pc, r_gp, r_sp, r_fp);
}

bool System::SetExpansionROM(const char* filename)
{
  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open '%s'", filename);
    return false;
  }

  std::fseek(fp, 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  std::vector<u8> data(size);
  if (std::fread(data.data(), size, 1, fp) != 1)
  {
    Log_ErrorPrintf("Failed to read ROM data from '%s'", filename);
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  Log_InfoPrintf("Loaded expansion ROM from '%s': %u bytes", filename, size);
  m_bus->SetExpansionROM(std::move(data));
  return true;
}

void System::Synchronize()
{
  const TickCount pending_ticks = m_cpu->GetPendingTicks();
  if (pending_ticks == 0)
    return;

  m_cpu->ResetPendingTicks();
  m_cpu->ResetDowncount();

  m_global_tick_counter += static_cast<u32>(pending_ticks);

  m_gpu->Execute(pending_ticks);
  m_timers->Execute(pending_ticks);
  m_cdrom->Execute(pending_ticks);
  m_pad->Execute(pending_ticks);
  m_spu->Execute(pending_ticks);
  m_mdec->Execute(pending_ticks);
  m_dma->Execute(pending_ticks);
}

void System::SetDowncount(TickCount downcount)
{
  m_cpu->SetDowncount(downcount);
}

void System::StallCPU(TickCount ticks)
{
  m_cpu->AddPendingTicks(ticks);
}

void System::SetController(u32 slot, std::shared_ptr<PadDevice> dev)
{
  m_pad->SetController(slot, std::move(dev));
}

void System::UpdateMemoryCards()
{
  m_pad->SetMemoryCard(0, nullptr);
  m_pad->SetMemoryCard(1, nullptr);

  const Settings& settings = m_host_interface->GetSettings();
  if (!settings.memory_card_a_path.empty())
  {
    std::shared_ptr<MemoryCard> card = MemoryCard::Open(this, settings.memory_card_a_path);
    if (card)
      m_pad->SetMemoryCard(0, std::move(card));
  }

  if (!settings.memory_card_b_path.empty())
  {
    std::shared_ptr<MemoryCard> card = MemoryCard::Open(this, settings.memory_card_b_path);
    if (card)
      m_pad->SetMemoryCard(1, std::move(card));
  }
}

bool System::HasMedia() const
{
  return m_cdrom->HasMedia();
}

bool System::InsertMedia(const char* path)
{
  std::unique_ptr<CDImage> image = CDImage::Open(path);
  if (!image)
    return false;

  m_cdrom->InsertMedia(std::move(image));
  return true;
}

void System::RemoveMedia()
{
  m_cdrom->RemoveMedia();
}
