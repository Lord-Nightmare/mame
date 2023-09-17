// license:BSD-3-Clause
// copyright-holders:smf
#include "emu.h"
#include "t10mmc.h"

#include "multibyte.h"

static int to_msf(int frame)
{
	int m = frame / (75 * 60);
	int s = (frame / 75) % 60;
	int f = frame % 75;

	return (m << 16) | (s << 8) | f;
}

void t10mmc::set_model(std::string model_name)
{
	m_model_name = model_name;
	while(m_model_name.size() < 28)
		m_model_name += ' ';
}

void t10mmc::t10_start(device_t &device)
{
	m_device = &device;
	t10spc::t10_start(device);

	device.save_item(NAME(m_lba));
	device.save_item(NAME(m_blocks));
	device.save_item(NAME(m_last_lba));
	device.save_item(NAME(m_num_subblocks));
	device.save_item(NAME(m_cur_subblock));
	device.save_item(NAME(m_audio_sense));
	device.save_item(NAME(m_sotc));
	device.save_item(NAME(m_read_cd_flags));
}

void t10mmc::t10_reset()
{
	t10spc::t10_reset();

	if( !m_image->exists() )
	{
		m_device->logerror( "T10MMC %s: no CD found!\n", m_image->tag() );
	}

	m_lba = 0;
	m_blocks = 0;
	m_last_lba = 0;
	m_sector_bytes = 2048;
	m_num_subblocks = 1;
	m_cur_subblock = 0;
	m_audio_sense = 0;
	m_sotc = 0;
	m_read_cd_flags = 0;
}

// scsicd_exec_command

void t10mmc::abort_audio()
{
	if (m_cdda->audio_active())
	{
		m_cdda->stop_audio();
		m_audio_sense = SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_STOPPED_DUE_TO_ERROR;
	}
}

t10mmc::toc_format_t t10mmc::toc_format()
{
	int mmc_format = command[2] & 0xf;
	if (mmc_format != 0)
	{
		return (toc_format_t) mmc_format;
	}

	/// SFF8020 legacy format field (see T10/1836-D Revision 2g page 643)
	return (toc_format_t) ((command[9] >> 6) & 3);
}

int t10mmc::toc_tracks()
{
	int start_track = command[6];
	int end_track = m_image->get_last_track();

	if (start_track == 0)
	{
		return end_track + 1;
	}
	else if (start_track <= end_track)
	{
		return ( end_track - start_track ) + 2;
	}
	else if (start_track <= 0xaa)
	{
		return 1;
	}

	return 0;
}

//
// Execute a SCSI command.

void t10mmc::ExecCommand()
{
	int trk;

	// keep updating the sense data while playing audio.
	if (command[0] == T10SPC_CMD_REQUEST_SENSE && m_audio_sense != SCSI_SENSE_ASC_ASCQ_NO_SENSE && m_sense_key == SCSI_SENSE_KEY_NO_SENSE && m_sense_asc == 0 && m_sense_ascq == 0)
	{
		if (m_audio_sense == SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS && !m_cdda->audio_active())
		{
			m_audio_sense = SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_SUCCESSFULLY_COMPLETED;
		}

		set_sense(SCSI_SENSE_KEY_NO_SENSE, (sense_asc_ascq_t) m_audio_sense);

		if (m_audio_sense != SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS)
		{
			m_audio_sense = SCSI_SENSE_ASC_ASCQ_NO_SENSE;
		}
	}

	switch ( command[0] )
	{
	case T10SPC_CMD_INQUIRY:
		//m_device->logerror("T10MMC: INQUIRY\n");
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT8( &command[ 4 ] );
		if (m_transfer_length > 36)
			m_transfer_length = 36;
		break;

	case T10SPC_CMD_MODE_SELECT_6:
		//m_device->logerror("T10MMC: MODE SELECT(6) length %x control %x\n", command[4], command[5]);
		m_phase = SCSI_PHASE_DATAOUT;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT8( &command[ 4 ] );
		break;

	case T10SPC_CMD_MODE_SENSE_6:
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT8( &command[ 4 ] );
		break;

	case T10SPC_CMD_START_STOP_UNIT:
		abort_audio();
		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10SPC_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10SBC_CMD_READ_CAPACITY:
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 8;
		break;

	case T10MMC_CMD_READ_DISC_STRUCTURE:
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = get_u16be(&command[8]);
		break;

	case T10SBC_CMD_READ_10:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		m_lba = get_u32be(&command[2]);
		m_blocks = SCSILengthFromUINT16( &command[7] );

		//m_device->logerror("T10MMC: READ(10) at LBA %x for %d blocks (%d bytes)\n", m_lba, m_blocks, m_blocks * m_sector_bytes);

		if (m_num_subblocks > 1)
		{
			m_cur_subblock = m_lba % m_num_subblocks;
			m_lba /= m_num_subblocks;
		}
		else
		{
			m_cur_subblock = 0;
		}

		abort_audio();

		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = m_blocks * m_sector_bytes;
		break;

	case T10MMC_CMD_READ_SUB_CHANNEL:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		//m_device->logerror("T10MMC: READ SUB-CHANNEL type %d\n", command[3]);
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT16( &command[ 7 ] );
		break;

	case T10MMC_CMD_READ_TOC_PMA_ATIP:
	{
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		int length;

		switch (toc_format())
		{
		case TOC_FORMAT_TRACKS:
			length = 4 + (8 * toc_tracks());
			break;

		case TOC_FORMAT_SESSIONS:
			length = 4 + (8 * 1);
			break;

		default:
			m_device->logerror("T10MMC: Unhandled READ TOC format %d\n", toc_format());
			length = 0;
			break;
		}

		int allocation_length = SCSILengthFromUINT16( &command[ 7 ] );

		if( length > allocation_length )
		{
			length = allocation_length;
		}

		abort_audio();

		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = length;
		break;
	}
	case T10MMC_CMD_PLAY_AUDIO_10:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		m_lba = get_u32be(&command[2]);
		m_blocks = SCSILengthFromUINT16( &command[7] );

		if (m_lba == 0)
		{
			// A request for LBA 0 will return something different depending on the type of media being played.
			// For data and mixed media, LBA 0 is assigned to MSF 00:02:00 (= LBA 150).
			// For audio media, LBA 0 is assigned to the actual starting address of track 1.
			if (m_image->get_track_type(0) == cdrom_file::CD_TRACK_AUDIO)
				m_lba = m_image->get_track_start(0);
			else
				m_lba = 150;
		}
		else if (m_lba == 0xffffffff)
		{
			m_device->logerror("T10MMC: play audio from current not implemented!\n");
		}

		//m_device->logerror("T10MMC: PLAY AUDIO(10) at LBA %x for %x blocks\n", m_lba, m_blocks);

		trk = m_image->get_track(m_lba);

		if (m_image->get_track_type(trk) == cdrom_file::CD_TRACK_AUDIO)
		{
			m_cdda->start_audio(m_lba, m_blocks);
			m_audio_sense = SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS;
		}
		else
		{
			m_device->logerror("T10MMC: track is NOT audio!\n");
			set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_MODE_FOR_THIS_TRACK);
		}

		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10MMC_CMD_PLAY_AUDIO_MSF:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		m_lba = (command[5] % 75) + ((command[4] * 75) % (60*75)) + (command[3] * (75*60));
		m_blocks = (command[8] % 75) + ((command[7] * 75) % (60*75)) + (command[6] * (75*60)) - m_lba;

		if (m_lba == 0)
		{
			if (m_image->get_track_type(0) == cdrom_file::CD_TRACK_AUDIO)
				m_lba = m_image->get_track_start(0);
			else
				m_lba = 150;
		}
		else if (m_lba == 0xffffffff)
		{
			m_device->logerror("T10MMC: play audio from current not implemented!\n");
		}

		//m_device->logerror("T10MMC: PLAY AUDIO MSF at LBA %x for %x blocks (MSF %i:%i:%i - %i:%i:%i)\n",
			//m_lba, m_blocks, command[3], command[4], command[5], command[6], command[7], command[8]);

		trk = m_image->get_track(m_lba);

		if (m_image->get_track_type(trk) == cdrom_file::CD_TRACK_AUDIO)
		{
			m_cdda->start_audio(m_lba, m_blocks);
			m_audio_sense = SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS;
		}
		else
		{
			m_device->logerror("T10MMC: track is NOT audio!\n");
			set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_MODE_FOR_THIS_TRACK);
		}

		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10MMC_CMD_PLAY_AUDIO_TRACK_INDEX:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		// [4] track start
		// [5] index start
		// [7] track end
		// [8] index end
		if (command[4] > command[7])
		{
			// TODO: check error
			set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_STOPPED_DUE_TO_ERROR);
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;

			m_device->logerror("Error: start TNO (%d,%d) > end TNO (%d,%d)\n", command[4], command[5], command[7], command[8]);
		}
		else
		{
			// be careful: tracks here are zero-based, but the SCSI command
			// uses the real CD track number which is 1-based!
			//m_device->logerror("T10MMC: PLAY AUDIO T/I: strk %d idx %d etrk %d idx %d frames %d\n", command[4], command[5], command[7], command[8], m_blocks);
			int end_track = m_image->get_last_track();
			if (end_track > command[7])
				end_track = command[7];

			// konamigv lacrazyc just sends same track start/end
			if (command[4] != command[7] && command[5] != command[8])
			{
				// HACK: assume index 0 & 1 means beginning of track and anything else means end of track
				if (command[8] <= 1)
					end_track--;

				if (m_sotc)
					end_track = command[4];
			}

			m_lba = m_image->get_track_start(command[4] - 1);
			m_blocks = m_image->get_track_start(end_track) - m_lba;
			trk = m_image->get_track(m_lba);

			if (m_image->get_track_type(trk) == cdrom_file::CD_TRACK_AUDIO)
			{
				m_cdda->start_audio(m_lba, m_blocks);
				m_audio_sense = SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS;
				m_status_code = SCSI_STATUS_CODE_GOOD;
				m_device->logerror("Starting audio TNO %d LBA %d blocks %d\n", trk, m_lba, m_blocks);
			}
			else
			{
				m_device->logerror("T10MMC: track is NOT audio!\n");
				// TODO: check error
				set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_MODE_FOR_THIS_TRACK);
				m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			}

			m_phase = SCSI_PHASE_STATUS;
			m_transfer_length = 0;
		}
		break;

	case T10MMC_CMD_PAUSE_RESUME:
		if (m_image)
		{
			m_cdda->pause_audio((command[8] & 0x01) ^ 0x01);
		}

		//m_device->logerror("T10MMC: PAUSE/RESUME: %s\n", command[8]&1 ? "RESUME" : "PAUSE");
		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10MMC_CMD_STOP_PLAY_SCAN:
		abort_audio();

		//m_device->logerror("T10MMC: STOP_PLAY_SCAN\n");
		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10SPC_CMD_MODE_SELECT_10:
		//m_device->logerror("T10MMC: MODE SELECT length %x control %x\n", get_u16be(&command[7]), command[1]);
		m_phase = SCSI_PHASE_DATAOUT;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT16( &command[ 7 ] );
		break;

	case T10SPC_CMD_MODE_SENSE_10:
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = SCSILengthFromUINT16( &command[ 7 ] );
		break;

	case T10MMC_CMD_PLAY_AUDIO_12:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		m_lba = get_u32be(&command[2]);
		m_blocks = get_u32be(&command[6]);

		if (m_lba == 0)
		{
			if (m_image->get_track_type(0) == cdrom_file::CD_TRACK_AUDIO)
				m_lba = m_image->get_track_start(0);
			else
				m_lba = 150;
		}
		else if (m_lba == 0xffffffff)
		{
			m_device->logerror("T10MMC: play audio from current not implemented!\n");
		}

		//m_device->logerror("T10MMC: PLAY AUDIO(12) at LBA %x for %x blocks\n", m_lba, m_blocks);

		trk = m_image->get_track(m_lba);

		if (m_image->get_track_type(trk) == cdrom_file::CD_TRACK_AUDIO)
		{
			m_cdda->start_audio(m_lba, m_blocks);
			m_audio_sense = SCSI_SENSE_ASC_ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS;
		}
		else
		{
			m_device->logerror("T10MMC: track is NOT audio!\n");
			set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_MODE_FOR_THIS_TRACK);
		}

		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10SBC_CMD_READ_12:
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		m_lba = get_u32be(&command[2]);
		m_blocks = get_u24be(&command[7]);

		//m_device->logerror("T10MMC: READ(12) at LBA %x for %x blocks (%x bytes)\n", m_lba, m_blocks, m_blocks * m_sector_bytes);

		if (m_num_subblocks > 1)
		{
			m_cur_subblock = m_lba % m_num_subblocks;
			m_lba /= m_num_subblocks;
		}
		else
		{
			m_cur_subblock = 0;
		}

		abort_audio();

		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = m_blocks * m_sector_bytes;
		break;

	case T10MMC_CMD_SET_CD_SPEED:
		m_device->logerror("T10MMC: SET CD SPEED to %d kbytes/sec.\n", get_u16be(&command[2]));
		m_phase = SCSI_PHASE_STATUS;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		m_transfer_length = 0;
		break;

	case T10MMC_CMD_READ_CD:
	{
		if (!m_image->exists())
		{
			m_phase = SCSI_PHASE_STATUS;
			m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
			m_transfer_length = 0;
			break;
		}

		m_lba = get_u32be(&command[2]);
		m_blocks = get_u24be(&command[6]);
		m_read_cd_flags = command[9];
		m_transfer_length = 0;

		// m_device->logerror("T10MMC: READ CD start_lba[%08x] block_len[%06x] %02x %02x %02x %02x\n", m_lba, m_blocks, command[1], command[9], command[10], command[11]);

		if (command[10] != 0)
			m_device->logerror("T10MMC: READ CD requested sub-channel data which is not implemented %02x\n", command[10]);

		const int expected_sector_type = BIT(command[1], 2, 3);
		int last_track_type = -1;
		uint8_t last_read_cd_flags = 0;
		for (int lba = m_lba; lba < m_lba + m_blocks; lba++)
		{
			auto trk = m_image->get_track(lba);
			auto track_type = m_image->get_track_type(trk);

			// If there's a transition between CD data and CD audio anywhere in the requested range then return an error
			if ((last_track_type == cdrom_file::CD_TRACK_AUDIO && track_type != cdrom_file::CD_TRACK_AUDIO)
			|| (last_track_type != cdrom_file::CD_TRACK_AUDIO && track_type == cdrom_file::CD_TRACK_AUDIO))
			{
				set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_MODE_FOR_THIS_TRACK);

				m_phase = SCSI_PHASE_STATUS;
				m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
				m_transfer_length = 0;
				return;
			}

			// Must read the subheader to figure out what sector type it is exactly when dealing with these specific track types
			int mode2_form = 0;
			if (track_type == cdrom_file::CD_TRACK_MODE2_RAW || track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX)
			{
				uint8_t tmp_buffer[2352];
				const int submode_offset = track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX ? 2 : 0x12;

				if (!m_image->read_data(lba, tmp_buffer, cdrom_file::CD_TRACK_RAW_DONTCARE))
				{
					m_device->logerror("T10MMC: CD read error! (%08x)\n", lba);
					return;
				}

				mode2_form = BIT(tmp_buffer[submode_offset], 5) + 1;
			}

			// If the expected sector type field is set then all tracks within the specified range must be the same
			if ((expected_sector_type == T10MMC_READ_CD_SECTOR_TYPE_CDDA && track_type != cdrom_file::CD_TRACK_AUDIO)
			|| (expected_sector_type == T10MMC_READ_CD_SECTOR_TYPE_MODE1 && track_type != cdrom_file::CD_TRACK_MODE1 && track_type != cdrom_file::CD_TRACK_MODE1_RAW)
			|| (expected_sector_type == T10MMC_READ_CD_SECTOR_TYPE_MODE2 && track_type != cdrom_file::CD_TRACK_MODE2)
			|| (expected_sector_type == T10MMC_READ_CD_SECTOR_TYPE_MODE2_FORM1 && track_type != cdrom_file::CD_TRACK_MODE2_FORM1 && mode2_form != 1)
			|| (expected_sector_type == T10MMC_READ_CD_SECTOR_TYPE_MODE2_FORM2 && track_type != cdrom_file::CD_TRACK_MODE2_FORM2 && mode2_form != 2))
			{
				set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_MODE_FOR_THIS_TRACK);

				m_phase = SCSI_PHASE_STATUS;
				m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
				m_transfer_length = 0;
				return;
			}

			// No fields selected is a valid request but none of the rest of the verification code is required in that case
			if (m_read_cd_flags == 0)
			{
				last_track_type = track_type;
				continue;
			}

			// Check for illegal combinations
			// t10 mmc spec gives a table which shows illegal combinations and combinations that get demoted
			auto read_cd_flags = m_read_cd_flags & 0xf8;

			// CDDA tracks can only ever be user data or no data (0), requesting other fields is not illegal but will be demoted to just user data
			if (track_type == cdrom_file::CD_TRACK_AUDIO)
				read_cd_flags = read_cd_flags ? T10MMC_READ_CD_FIELD_USER_DATA : 0;

			// All of these combinations will be illegal for all tracks besides CDDA tracks
			bool is_illegal_combo = (read_cd_flags == (T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SUBHEADER | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_SUBHEADER | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_USER_DATA))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_USER_DATA | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_SUBHEADER))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_SUBHEADER | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_SUBHEADER | T10MMC_READ_CD_FIELD_USER_DATA))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_SUBHEADER | T10MMC_READ_CD_FIELD_USER_DATA | T10MMC_READ_CD_FIELD_ECC))
				|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_SUBHEADER | T10MMC_READ_CD_FIELD_ECC));

			// Mode 2 form 1/2 sectors have additional restrictions that CDDA, mode 1, and mode 2 formless tracks don't
			if (!is_illegal_combo && (track_type == cdrom_file::CD_TRACK_MODE2_FORM1 || track_type == cdrom_file::CD_TRACK_MODE2_FORM2 || mode2_form > 0))
			{
				is_illegal_combo = (read_cd_flags == (T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_USER_DATA))
					|| (read_cd_flags == (T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_USER_DATA | T10MMC_READ_CD_FIELD_ECC))
					|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_USER_DATA))
					|| (read_cd_flags == (T10MMC_READ_CD_FIELD_SYNC | T10MMC_READ_CD_FIELD_HEADER | T10MMC_READ_CD_FIELD_USER_DATA | T10MMC_READ_CD_FIELD_ECC));
			}

			// Mask out flags that can't be used for specific track types
			if (track_type == cdrom_file::CD_TRACK_MODE1 || track_type == cdrom_file::CD_TRACK_MODE1_RAW)
			{
				// Sub header only is valid but will return 0 bytes, otherwise subheader is always demoted
				if (read_cd_flags != T10MMC_READ_CD_FIELD_SUBHEADER)
					read_cd_flags &= ~T10MMC_READ_CD_FIELD_SUBHEADER;
			}
			else if (track_type == cdrom_file::CD_TRACK_MODE2)
			{
				// Mode 2 formless
				// No EDC/ECC data
				read_cd_flags &= ~T10MMC_READ_CD_FIELD_ECC;

				// Sub header only is valid but will return 0 bytes, otherwise subheader is always demoted
				if (read_cd_flags != T10MMC_READ_CD_FIELD_SUBHEADER)
					read_cd_flags &= ~T10MMC_READ_CD_FIELD_SUBHEADER;
			}
			else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM2 || mode2_form == 2)
			{
				// No EDC/ECC data
				read_cd_flags &= ~T10MMC_READ_CD_FIELD_ECC;
			}

			// Requested fields must be valid for all tracks within the selected range
			if (is_illegal_combo || (last_track_type != -1 && read_cd_flags != last_read_cd_flags))
			{
				m_device->logerror("T10MMC: READ CD called with invalid data request for given track type %d %02x\n", track_type, command[9]);
				set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_SENSE_ASC_ASCQ_ILLEGAL_FIELD_IN_CDB);
				m_phase = SCSI_PHASE_STATUS;
				m_status_code = SCSI_STATUS_CODE_CHECK_CONDITION;
				m_transfer_length = 0;
				return;
			}

			// The actual transfer size must be calculated for every possible valid requested data
			const int c2_error_codes = BIT(m_read_cd_flags, 1, 2);
			const bool requested_c2 = c2_error_codes == T10MMC_READ_CD_C2_ONLY;
			const bool requested_c2_error_block = c2_error_codes == T10MMC_READ_CD_C2_BLOCK;

			const bool requested_edc_ecc = (read_cd_flags & T10MMC_READ_CD_FIELD_ECC) != 0;
			const bool requested_user_data = (read_cd_flags & T10MMC_READ_CD_FIELD_USER_DATA) != 0;
			const bool requested_header = (read_cd_flags & T10MMC_READ_CD_FIELD_HEADER) != 0;
			const bool requested_subheader = (read_cd_flags & T10MMC_READ_CD_FIELD_SUBHEADER) != 0;
			const bool requested_sync = (read_cd_flags & T10MMC_READ_CD_FIELD_SYNC) != 0;

			if (requested_c2 || requested_c2_error_block)
				m_transfer_length += 294;

			if (requested_c2_error_block)
				m_transfer_length += 2;

			if (track_type == cdrom_file::CD_TRACK_AUDIO)
			{
				if (requested_user_data)
					m_transfer_length += 2352;
			}
			else if (track_type == cdrom_file::CD_TRACK_MODE1 || track_type == cdrom_file::CD_TRACK_MODE1_RAW)
			{
				if (requested_sync)
					m_transfer_length += 12;
				if (requested_header)
					m_transfer_length += 4;
				if (requested_user_data)
					m_transfer_length += 2048;
				if (requested_edc_ecc)
					m_transfer_length += 288;
			}
			else if (track_type == cdrom_file::CD_TRACK_MODE2)
			{
				if (requested_sync)
					m_transfer_length += 12;
				if (requested_header)
					m_transfer_length += 4;
				if (requested_user_data)
					m_transfer_length += 2336;
			}
			else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM1 || mode2_form == 1)
			{
				if (requested_sync)
					m_transfer_length += 12;
				if (requested_header)
					m_transfer_length += 4;
				if (requested_subheader)
					m_transfer_length += 8;
				if (requested_user_data)
					m_transfer_length += 2048;
				if (requested_edc_ecc)
					m_transfer_length += 280;
			}
			else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM2 || mode2_form == 2)
			{
				if (requested_sync)
					m_transfer_length += 12;
				if (requested_header)
					m_transfer_length += 4;
				if (requested_subheader)
					m_transfer_length += 8;
				if (requested_user_data)
					m_transfer_length += 2328;
			}

			last_track_type = track_type;
			last_read_cd_flags = read_cd_flags;
		}

		// Only worry about SGI block extension stuff if it ever becomes an issue
		if (m_num_subblocks > 1)
			m_device->logerror("T10MMC: READ CD does not handle sub blocks currently\n");

		// All fields were matched between all tracks, so store the simplified version
		m_read_cd_flags = last_read_cd_flags | (m_read_cd_flags & ~0xf8);

		m_cur_subblock = 0;
		m_phase = SCSI_PHASE_DATAIN;
		m_status_code = SCSI_STATUS_CODE_GOOD;
		break;
	}

	default:
		t10spc::ExecCommand();
	}
}

// scsicd_read_data
//
// Read data from the device resulting from the execution of a command

void t10mmc::ReadData( uint8_t *data, int dataLength )
{
	uint32_t temp;
	uint8_t tmp_buffer[2352];

	switch ( command[0] )
	{
	case T10SPC_CMD_INQUIRY:
		data[0] = 0x05; // device is present, device is CD/DVD (MMC-3)
		data[1] = 0x80; // media is removable
		data[2] = 0x05; // device complies with SPC-3 standard
		data[3] = 0x02; // response data format = SPC-3 standard
		data[4] = 0x1f;
		data[5] = 0;
		data[6] = 0;
		data[7] = 0;
		memcpy(&data[8], m_model_name.data(), 28);
		break;

	case T10SBC_CMD_READ_CAPACITY:
		m_device->logerror("T10MMC: READ CAPACITY\n");

		temp = m_image->get_track_start(0xaa);
		temp--; // return the last used block on the disc

		put_u32be(&data[0], temp);
		data[4] = 0;
		data[5] = 0;
		put_u16be(&data[6], m_sector_bytes);
		break;

	case T10SBC_CMD_READ_10:
	case T10SBC_CMD_READ_12:
		//m_device->logerror("T10MMC: read %x dataLength lba=%x\n", dataLength, m_lba);
		if ((m_image) && (m_blocks))
		{
			while (dataLength > 0)
			{
				if (!m_image->read_data(m_lba, tmp_buffer, cdrom_file::CD_TRACK_MODE1))
				{
					m_device->logerror("T10MMC: CD read error! (%08x)\n", m_lba);
					return;
				}

				//m_device->logerror("True LBA: %d, buffer half: %d\n", m_lba, m_cur_subblock * m_sector_bytes);

				memcpy(data, &tmp_buffer[m_cur_subblock * m_sector_bytes], m_sector_bytes);

				m_cur_subblock++;
				if (m_cur_subblock >= m_num_subblocks)
				{
					m_cur_subblock = 0;

					m_lba++;
					m_blocks--;
				}

				m_last_lba = m_lba;
				dataLength -= m_sector_bytes;
				data += m_sector_bytes;
			}
		}
		break;

	case T10MMC_CMD_READ_CD:
		//m_device->logerror("T10MMC: read CD %x dataLength lba=%x\n", dataLength, m_lba);
		if ((m_image) && (m_blocks))
		{
			const int c2_error_codes = BIT(m_read_cd_flags, 1, 2);
			const bool requested_c2 = c2_error_codes == T10MMC_READ_CD_C2_ONLY;
			const bool requested_c2_error_block = c2_error_codes == T10MMC_READ_CD_C2_BLOCK;

			const bool requested_edc_ecc = (m_read_cd_flags & T10MMC_READ_CD_FIELD_ECC) != 0;
			const bool requested_user_data = (m_read_cd_flags & T10MMC_READ_CD_FIELD_USER_DATA) != 0;
			const bool requested_header = (m_read_cd_flags & T10MMC_READ_CD_FIELD_HEADER) != 0;
			const bool requested_subheader = (m_read_cd_flags & T10MMC_READ_CD_FIELD_SUBHEADER) != 0;
			const bool requested_sync = (m_read_cd_flags & T10MMC_READ_CD_FIELD_SYNC) != 0;

			// m_device->logerror("T10MMC: read CD flags c2[%d] c2block[%d] edc/ecc[%d] user[%d] header[%d] subheader[%d] sync[%d]\n", requested_c2, requested_c2_error_block, requested_edc_ecc, requested_user_data, requested_header, requested_subheader, requested_sync);

			if (m_read_cd_flags == 0)
			{
				// No data is supposed to be returned
				if (dataLength > 0)
					memset(data, 0, dataLength);

				data += dataLength;
				dataLength = 0;
			}

			while (dataLength > 0)
			{
				int data_idx = 0;
				auto trk = m_image->get_track(m_lba);
				auto track_type = m_image->get_track_type(trk);

				// Some CHDs don't have the required data so just log the error and return zeros as required
				// CD_TRACK_MODE1: Only has user data
				// CD_TRACK_MODE1_RAW: Has all fields required
				// CD_TRACK_MODE2: Only has user data
				// CD_TRACK_MODE2_FORM1: ?
				// CD_TRACK_MODE2_FORM2: ?
				// CD_TRACK_MODE2_FORM_MIX: Subheader data + user data + EDC/ECC data
				// CD_TRACK_MODE2_RAW: Has all fields required
				// CD_TRACK_AUDIO: Only returns user data

				auto read_track_type = track_type;
				if (track_type == cdrom_file::CD_TRACK_MODE1)
					read_track_type = cdrom_file::CD_TRACK_MODE1_RAW; // mode1 has code for partial promotion to mode1_raw so make use of it

				if (!m_image->read_data(m_lba, tmp_buffer, read_track_type))
				{
					m_device->logerror("T10MMC: CD read error! (%08x)\n", m_lba);
					return;
				}

				int mode2_form = 0;
				if (track_type == cdrom_file::CD_TRACK_MODE2_RAW)
					mode2_form = BIT(tmp_buffer[0x12], 5) + 1;
				else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX)
					mode2_form = BIT(tmp_buffer[2], 5) + 1;

				if (requested_sync)
				{
					if (track_type == cdrom_file::CD_TRACK_MODE2 || track_type == cdrom_file::CD_TRACK_MODE2_FORM1 || track_type == cdrom_file::CD_TRACK_MODE2_FORM2 || track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX)
					{
						m_device->logerror("T10MMC: sync data is not available for track type %d, inserting fake sync data\n", track_type);
						constexpr uint8_t sync_field[] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
						memcpy(data + data_idx, sync_field, std::size(sync_field));
					}
					else
						memcpy(data + data_idx, tmp_buffer, 12);

					data_idx += 12;
				}

				if (requested_header)
				{
					if (track_type == cdrom_file::CD_TRACK_MODE2 || track_type == cdrom_file::CD_TRACK_MODE2_FORM1 || track_type == cdrom_file::CD_TRACK_MODE2_FORM2 || track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX)
					{
						m_device->logerror("T10MMC: header data is not available for track type %d, inserting fake header data\n", track_type);

						uint32_t msf = to_msf(m_lba);
						data[data_idx] = BIT(msf, 16, 8);
						data[data_idx+1] = BIT(msf, 8, 8);
						data[data_idx+2] = BIT(msf, 0, 8);
						data[data_idx+3] = 2; // mode 2
					}
					else
						memcpy(data + data_idx, tmp_buffer + 12, 4);

					data_idx += 4;
				}

				if (requested_subheader)
				{
					if (track_type == cdrom_file::CD_TRACK_MODE2_RAW)
					{
						memcpy(data + data_idx, tmp_buffer + 16, 8);
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX)
					{
						// Only been able to verify 1 CHD for form mix, appears to have the 8 subheader bytes at the top of the sector
						memcpy(data + data_idx, tmp_buffer, 8);
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM1 || track_type == cdrom_file::CD_TRACK_MODE2_FORM2)
					{
						// It's not possible to generate a fake subheader for mode 2 tracks because the submode byte contains detailed info
						// about what actual data is inside the block
						m_device->logerror("T10MMC: subheader data is not available for track type %d\n", track_type);
						memset(data + data_idx, 0, 8);

						if (track_type == cdrom_file::CD_TRACK_MODE2_FORM2)
							data[data_idx+2] = data[data_idx+6] = 1 << 5; // The form 2 flag can at least be set accurately
					}

					// Mode 1 and mode 2 formless return 0 bytes
					if (track_type != cdrom_file::CD_TRACK_MODE1 && track_type != cdrom_file::CD_TRACK_MODE1_RAW && track_type != cdrom_file::CD_TRACK_MODE2)
						data_idx += 8;
				}

				if (requested_user_data)
				{
					int buffer_offset = 0;
					int data_len = 0;

					if (track_type == cdrom_file::CD_TRACK_AUDIO)
					{
						buffer_offset = 0;
						data_len = 2352;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE1)
					{
						buffer_offset = 0;
						data_len = 2048;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE1_RAW)
					{
						buffer_offset = 16;
						data_len = 2048;
					}
					else if ((track_type == cdrom_file::CD_TRACK_MODE2_RAW && mode2_form == 1))
					{
						buffer_offset = 24;
						data_len = 2048;
					}
					else if ((track_type == cdrom_file::CD_TRACK_MODE2_RAW && mode2_form == 2))
					{
						buffer_offset = 24;
						data_len = 2328;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX && mode2_form == 1)
					{
						buffer_offset = 8;
						data_len = 2048;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX && mode2_form == 2)
					{
						buffer_offset = 8;
						data_len = 2328;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2)
					{
						// Untested
						buffer_offset = 0;
						data_len = 2336;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM1)
					{
						// Untested
						m_device->logerror("T10MMC: reading user data from untested mode 2 form 1 track\n");
						buffer_offset = 0;
						data_len = 2048;
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM2)
					{
						// Untested
						m_device->logerror("T10MMC: reading user data from untested mode 2 form 2 track\n");
						buffer_offset = 0;
						data_len = 2324;
					}

					memcpy(data + data_idx, tmp_buffer + buffer_offset, data_len);
					data_idx += data_len;

					if (track_type == cdrom_file::CD_TRACK_MODE2_FORM2)
					{
						// Untested, but the sector size of 2324 as noted in cdrom.h
						// implies the lack of the last 4 bytes for the (optional) CRC
						memset(data + data_idx, 0, 4);
						data_idx += 4;
					}
				}

				if (requested_edc_ecc)
				{
					if (track_type == cdrom_file::CD_TRACK_MODE1_RAW)
					{
						// Includes the 8 bytes of padding
						memcpy(data + data_idx, tmp_buffer + 16 + 2048, 288);
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_RAW && mode2_form == 1)
					{
						memcpy(data + data_idx, tmp_buffer + 24 + 2048, 280);
					}
					else if (track_type == cdrom_file::CD_TRACK_MODE2_FORM_MIX && mode2_form == 1)
					{
						memcpy(data + data_idx, tmp_buffer + 8 + 2048, 280);
					}
					else
					{
						m_device->logerror("T10MMC: EDC/ECC data is not available for track type %d\n", track_type);
						memset(data + data_idx, 0, 280);
					}

					data_idx += 280;
				}

				if (requested_c2 || requested_c2_error_block)
				{
					m_device->logerror("T10MMC: C2 field is not supported, returning zero data\n");
					memset(data + data_idx, 0, 294);
					data_idx += 294;
				}

				if (requested_c2_error_block)
				{
					m_device->logerror("T10MMC: error block field is not supported, returning zero data\n");
					memset(data + data_idx, 0, 2);
					data_idx += 2;
				}

				m_lba++;
				m_blocks--;

				m_last_lba = m_lba;
				dataLength -= data_idx;
				data += data_idx;
				data_idx = 0;
			}
		}
		break;

	case T10MMC_CMD_READ_SUB_CHANNEL:
		switch (command[3])
		{
			case 1: // return current position
			{
				if (!m_image)
				{
					return;
				}

				m_device->logerror("T10MMC: READ SUB-CHANNEL Time = %x, SUBQ = %x\n", command[1], command[2]);

				bool msf = (command[1] & 0x2) != 0;

				data[0]= 0x00;

				int audio_active = m_cdda->audio_active();
				if (audio_active)
				{
					// if audio is playing, get the latest LBA from the CDROM layer
					m_last_lba = m_cdda->get_audio_lba();
					if (m_cdda->audio_paused())
					{
						data[1] = 0x12;     // audio is paused
					}
					else
					{
						data[1] = 0x11;     // audio in progress
					}
				}
				else
				{
					m_last_lba = 0;
					if (m_cdda->audio_ended())
					{
						data[1] = 0x13; // ended successfully
					}
					else
					{
//                          data[1] = 0x14;    // stopped due to error
						data[1] = 0x15; // No current audio status to return
					}
				}

				if (command[2] & 0x40)
				{
					data[2] = 0;
					data[3] = 12;       // data length
					data[4] = 0x01; // sub-channel format code
					data[5] = 0x10 | (audio_active ? 0 : 4);
					data[6] = m_image->get_track(m_last_lba) + 1; // track
					data[7] = 0;    // index

					uint32_t frame = m_last_lba;

					if (msf)
					{
						frame = to_msf(frame);
					}

					put_u32be(&data[8], frame);

					frame = m_last_lba - m_image->get_track_start(data[6] - 1);

					if (msf)
					{
						frame = to_msf(frame);
					}

					put_u32be(&data[12], frame);
				}
				else
				{
					data[2] = 0;
					data[3] = 0;
				}
				break;
			}
			default:
				m_device->logerror("T10MMC: Unknown subchannel type %d requested\n", command[3]);
		}
		break;

	case T10MMC_CMD_READ_TOC_PMA_ATIP:
		/*
		    Track numbers are problematic here: 0 = lead-in, 0xaa = lead-out.
		    That makes sense in terms of how real-world CDs are referred to, but
		    our internal routines for tracks use "0" as track 1.  That probably
		    should be fixed...
		*/
		{
			bool msf = (command[1] & 0x2) != 0;

			m_device->logerror("T10MMC: READ TOC, format = %d time=%d\n", toc_format(),msf);
			switch (toc_format())
			{
			case TOC_FORMAT_TRACKS:
				{
					int tracks = toc_tracks();
					int len = 2 + (tracks * 8);

					// the returned TOC DATA LENGTH must be the full amount,
					// regardless of how much we're able to pass back due to in_len
					int dptr = 0;
					put_u16be(&data[dptr], len);
					dptr += 2;
					data[dptr++] = 1;
					data[dptr++] = m_image->get_last_track();

					int first_track = command[6];
					if (first_track == 0)
					{
						first_track = 1;
					}

					for (int i = 0; i < tracks; i++)
					{
						int track = first_track + i;
						int cdrom_track = track - 1;
						if( i == tracks - 1 )
						{
							track = 0xaa;
							cdrom_track = 0xaa;
						}

						if( dptr >= dataLength )
						{
							break;
						}

						data[dptr++] = 0;
						data[dptr++] = m_image->get_adr_control(cdrom_track);
						data[dptr++] = track;
						data[dptr++] = 0;

						uint32_t tstart = m_image->get_track_start(cdrom_track);

						if (msf)
						{
							tstart = to_msf(tstart+150);
						}

						put_u32be(&data[dptr], tstart);
						dptr += 4;
					}
				}
				break;

			case TOC_FORMAT_SESSIONS:
				{
					int len = 2 + (8 * 1);

					int dptr = 0;
					put_u16be(&data[dptr], len);
					dptr += 2;
					data[dptr++] = 1;
					data[dptr++] = 1;

					data[dptr++] = 0;
					data[dptr++] = m_image->get_adr_control(0);
					data[dptr++] = 1;
					data[dptr++] = 0;

					uint32_t tstart = m_image->get_track_start(0);

					if (msf)
					{
						tstart = to_msf(tstart+150);
					}

					put_u32be(&data[dptr], tstart);
					dptr += 4;
				}
				break;

			default:
				m_device->logerror("T10MMC: Unhandled READ TOC format %d\n", toc_format());
				break;
			}
		}
		break;

	case T10SPC_CMD_MODE_SENSE_6:
	case T10SPC_CMD_MODE_SENSE_10:
		//      m_device->logerror("T10MMC: MODE SENSE page code = %x, PC = %x\n", command[2] & 0x3f, (command[2]&0xc0)>>6);
		{
			memset(data, 0, SCSILengthFromUINT16( &command[ 7 ] ));

			const uint8_t page = command[2] & 0x3f;
			int ptr = 0;

			if ((page == 0xe) || (page == 0x3f))
			{ // CD Audio control page
					data[ptr++] = 0x8e; // page E, parameter is savable
					data[ptr++] = 0x0e; // page length
					data[ptr++] = (1 << 2) | (m_sotc << 1); // IMMED = 1
					ptr += 6;   // skip reserved bytes
					// connect each audio channel to 1 output port
					data[ptr++] = 1;
					data[ptr++] = 2;
					data[ptr++] = 4;
					data[ptr++] = 8;
					// indicate max volume
					data[ptr++] = 0xff;
					ptr++;
					data[ptr++] = 0xff;
					ptr++;
					data[ptr++] = 0xff;
					ptr++;
					data[ptr++] = 0xff;
			}

			if ((page == 0x0d) || (page == 0x3f))
			{ // CD page
					data[ptr++] = 0x0d;
					data[ptr++] = 6;    // page length
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 60;
					data[ptr++] = 0;
					data[ptr++] = 75;
			}

			if ((page == 0x2a) || (page == 0x3f))
			{ // Page capabilities
					data[ptr++] = 0x2a;
					data[ptr++] = 0x14; // page length
					data[ptr++] = 0x00;
					data[ptr++] = 0x00; // CD-R only
					data[ptr++] = 0x01; // can play audio
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0x02;
					data[ptr++] = 0xc0; // 4x speed
					data[ptr++] = 0x01;
					data[ptr++] = 0x00; // 256 volume levels supported
					data[ptr++] = 0x00;
					data[ptr++] = 0x00; // buffer
					data[ptr++] = 0x02;
					data[ptr++] = 0xc0; // 4x read speed
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0;
					data[ptr++] = 0;
			}
		}
		break;

	case T10MMC_CMD_READ_DISC_STRUCTURE:
		m_device->logerror("T10MMC: READ DISC STRUCTURE, data\n");
		data[0] = data[1] = 0;
		data[2] = data[3] = 0;

		if((command[1] & 0x0f) == 0 && command[7] == 0x04) // DVD / DVD disc manufacturing information
		{
			data[1] = 0xe;
			for(int i=4; i != 0xe; i++)
				data[i] = 0;
		}
		break;

	default:
		t10spc::ReadData( data, dataLength );
		break;
	}
}

// scsicd_write_data
//
// Write data to the CD-ROM device as part of the execution of a command

void t10mmc::WriteData( uint8_t *data, int dataLength )
{
	switch (command[ 0 ])
	{
	case T10SPC_CMD_MODE_SELECT_6:
	case T10SPC_CMD_MODE_SELECT_10:
		m_device->logerror("T10MMC: MODE SELECT page %x\n", data[0] & 0x3f);

		switch (data[0] & 0x3f)
		{
			case 0x0:   // vendor-specific
				// check for SGI extension to force 512-byte blocks
				if ((data[3] == 8) && (data[10] == 2))
				{
					m_device->logerror("T10MMC: Experimental SGI 512-byte block extension enabled\n");

					m_sector_bytes = 512;
					m_num_subblocks = 4;
				}
				else
				{
					m_device->logerror("T10MMC: Unknown vendor-specific page!\n");
				}
				break;

			case 0xe:   // audio page
				m_sotc = (data[2] >> 1) & 1;
				m_device->logerror("Ch 0 route: %x vol: %x\n", data[8], data[9]);
				m_device->logerror("Ch 1 route: %x vol: %x\n", data[10], data[11]);
				m_device->logerror("Ch 2 route: %x vol: %x\n", data[12], data[13]);
				m_device->logerror("Ch 3 route: %x vol: %x\n", data[14], data[15]);
				m_cdda->set_output_gain(0, data[9] / 255.0f);
				m_cdda->set_output_gain(1, data[11] / 255.0f);
				break;
		}
		break;

	default:
		t10spc::WriteData( data, dataLength );
		break;
}
}
