#include "assert.h"
#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <libcue/libcue.h>
#include <map>
Log_SetChannel(CDImageMemory);

class CDImageMemory : public CDImage
{
public:
  CDImageMemory();
  ~CDImageMemory() override;

  bool CopyImage(CDImage* image, ProgressCallback* progress);

  bool ReadSubChannelQ(SubChannelQ* subq) override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  u8* m_memory = nullptr;
  u32 m_memory_sectors = 0;
  CDSubChannelReplacement m_sbi;
};

CDImageMemory::CDImageMemory() = default;

CDImageMemory::~CDImageMemory()
{
  if (m_memory)
    std::free(m_memory);
}

bool CDImageMemory::CopyImage(CDImage* image, ProgressCallback* progress)
{
  // figure out the total number of sectors (not including blank pregaps)
  m_memory_sectors = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    const Index& index = image->GetIndex(i);
    if (index.file_sector_size > 0)
      m_memory_sectors += image->GetIndex(i).length;
  }

  if ((static_cast<u64>(RAW_SECTOR_SIZE) * static_cast<u64>(m_memory_sectors)) >=
      static_cast<u64>(std::numeric_limits<size_t>::max()))
  {
    progress->DisplayFormattedModalError("Insufficient address space");
    return false;
  }

  progress->SetFormattedStatusText("Allocating memory for %u sectors...", m_memory_sectors);

  m_memory =
    static_cast<u8*>(std::malloc(static_cast<size_t>(RAW_SECTOR_SIZE) * static_cast<size_t>(m_memory_sectors)));
  if (!m_memory)
  {
    progress->DisplayFormattedModalError("Failed to allocate memory for %llu sectors", m_memory_sectors);
    return false;
  }

  progress->SetStatusText("Preloading CD image to RAM...");
  progress->SetProgressRange(m_memory_sectors);
  progress->SetProgressValue(0);

  u8* memory_ptr = m_memory;
  u32 sectors_read = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    const Index& index = image->GetIndex(i);
    if (index.file_sector_size == 0)
      continue;

    for (u32 lba = 0; lba < index.length; lba++)
    {
      if (!image->ReadSectorFromIndex(memory_ptr, index, lba))
      {
        Log_ErrorPrintf("Failed to read LBA %u in index %u", lba, index);
        return false;
      }

      progress->SetProgressValue(sectors_read);
      memory_ptr += RAW_SECTOR_SIZE;
      sectors_read++;
    }
  }

  for (u32 i = 1; i <= image->GetTrackCount(); i++)
    m_tracks.push_back(image->GetTrack(i));

  u32 current_offset = 0;
  for (u32 i = 0; i < image->GetIndexCount(); i++)
  {
    Index new_index = image->GetIndex(i);
    new_index.file_index = 0;
    if (new_index.file_sector_size > 0)
    {
      new_index.file_offset = current_offset;
      current_offset += new_index.length;
    }
    m_indices.push_back(new_index);
  }

  Assert(current_offset == m_memory_sectors);
  m_filename = image->GetFileName();
  m_lba_count = image->GetLBACount();

  if (!image->Seek(0))
  {
    progress->ModalError("Failed to seek to start of image for subq read");
    return false;
  }

  progress->SetStatusText("Looking for invalid subchannel data...");

  CDImage::SubChannelQ subq;
  for (LBA lba = 0; lba < m_lba_count; lba++)
  {
    if (ReadSubChannelQ(&subq) && !subq.IsCRCValid())
      m_sbi.AddReplacementSubChannelQ(lba, subq);
  }

  return Seek(1, Position{0, 0, 0});
}

bool CDImageMemory::ReadSubChannelQ(SubChannelQ* subq)
{
  if (m_sbi.GetReplacementSubChannelQ(m_position_on_disc, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq);
}

bool CDImageMemory::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  DebugAssert(index.file_index == 0);

  const u64 sector_number = index.file_offset + lba_in_index;
  if (sector_number >= m_memory_sectors)
    return false;

  const size_t file_offset = static_cast<size_t>(sector_number) * static_cast<size_t>(RAW_SECTOR_SIZE);
  std::memcpy(buffer, &m_memory[file_offset], RAW_SECTOR_SIZE);
  return true;
}

std::unique_ptr<CDImage>
CDImage::CreateMemoryImage(CDImage* image, ProgressCallback* progress /* = ProgressCallback::NullProgressCallback */)
{
  std::unique_ptr<CDImageMemory> memory_image = std::make_unique<CDImageMemory>();
  if (!memory_image->CopyImage(image, progress))
    return {};

  return memory_image;
}
