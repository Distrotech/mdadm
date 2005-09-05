/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2002 Neil Brown <neilb@cse.unsw.edu.au>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@cse.unsw.edu.au>
 *    Paper: Neil Brown
 *           School of Computer Science and Engineering
 *           The University of New South Wales
 *           Sydney, 2052
 *           Australia
 */

#include	"mdadm.h"
#include	"md_u.h"
#include	"md_p.h"

int Assemble(char *mddev, int mdfd,
	     mddev_ident_t ident, char *conffile,
	     mddev_dev_t devlist,
	     int readonly, int runstop,
	     char *update,
	     int verbose, int force)
{
	/*
	 * The task of Assemble is to find a collection of
	 * devices that should (according to their superblocks)
	 * form an array, and to give this collection to the MD driver.
	 * In Linux-2.4 and later, this involves submitting a
	 * SET_ARRAY_INFO ioctl with no arg - to prepare
	 * the array - and then submit a number of
	 * ADD_NEW_DISK ioctls to add disks into
	 * the array.  Finally RUN_ARRAY might
	 * be submitted to start the array.
	 *
	 * Much of the work of Assemble is in finding and/or
	 * checking the disks to make sure they look right.
	 *
	 * If mddev is not set, then scan must be and we
	 *  read through the config file for dev+uuid mapping
	 *  We recurse, setting mddev, for each device that
	 *    - isn't running
	 *    - has a valid uuid (or any uuid if !uuidset
	 *
	 * If mddev is set, we try to determine state of md.
	 *   check version - must be at least 0.90.0
	 *   check kernel version.  must be at least 2.4.
	 *    If not, we can possibly fall back on START_ARRAY
	 *   Try to GET_ARRAY_INFO.
	 *     If possible, give up
	 *     If not, try to STOP_ARRAY just to make sure
	 *
	 * If !uuidset and scan, look in conf-file for uuid
	 *       If not found, give up
	 * If !devlist and scan and uuidset, get list of devs from conf-file 
	 *
	 * For each device:
	 *   Check superblock - discard if bad
	 *   Check uuid (set if we don't have one) - discard if no match
	 *   Check superblock similarity if we have a superblock - discard if different
	 *   Record events, devicenum, utime
	 * This should give us a list of devices for the array
	 * We should collect the most recent event and utime numbers
	 *
	 * Count disks with recent enough event count
	 * While force && !enough disks
	 *    Choose newest rejected disks, update event count
	 *     mark clean and rewrite superblock
	 * If recent kernel:
	 *    SET_ARRAY_INFO
	 *    foreach device with recent events : ADD_NEW_DISK
	 *    if runstop == 1 || "enough" disks and runstop==0 -> RUN_ARRAY
	 * If old kernel:
	 *    Check the device numbers in superblock are right
	 *    update superblock if any changes
	 *    START_ARRAY
	 *
	 */
	int old_linux = 0;
	int vers;
	mdu_array_info_t array;
	mdp_super_t first_super, super;
	struct {
		char *devname;
		unsigned int major, minor;
		unsigned int oldmajor, oldminor;
		long long events;
		time_t utime;
		int uptodate;
		int state;
		int raid_disk;
	} *devices;
	int *best = NULL; /* indexed by raid_disk */
	unsigned int bestcnt = 0;
	int devcnt = 0;
	unsigned int okcnt, sparecnt;
	unsigned int req_cnt;
	unsigned int i;
	int most_recent = 0;
	int chosen_drive;
	int change = 0;
	int inargv = 0;
	int start_partial_ok = force || devlist==NULL;
	unsigned int num_devs;
	mddev_dev_t tmpdev;
	char *avail;
	
	vers = md_get_version(mdfd);
	if (vers <= 0) {
		fprintf(stderr, Name ": %s appears not to be an md device.\n", mddev);
		return 1;
	}
	if (vers < 9000) {
		fprintf(stderr, Name ": Assemble requires driver version 0.90.0 or later.\n"
			"    Upgrade your kernel or try --build\n");
		return 1;
	}
	if (get_linux_version() < 2004000)
		old_linux = 1;

	if (ioctl(mdfd, GET_ARRAY_INFO, &array)>=0) {
		fprintf(stderr, Name ": device %s already active - cannot assemble it\n",
			mddev);
		return 1;
	}
	ioctl(mdfd, STOP_ARRAY, NULL); /* just incase it was started but has no content */

	/*
	 * If any subdevs are listed, then any that don't
	 * match ident are discarded.  Remainder must all match and
	 * become the array.
	 * If no subdevs, then we scan all devices in the config file, but
	 * there must be something in the identity
	 */

	if (!devlist &&
	    ident->uuid_set == 0 &&
	    ident->super_minor < 0 &&
	    ident->devices == NULL) {
		fprintf(stderr, Name ": No identity information available for %s - cannot assemble.\n",
			mddev);
		return 1;
	}
	if (devlist == NULL)
		devlist = conf_get_devs(conffile);
	else inargv = 1;

	tmpdev = devlist; num_devs = 0;
	while (tmpdev) {
		num_devs++;
		tmpdev = tmpdev->next;
	}
	best = malloc(num_devs * sizeof(*best));
	devices = malloc(num_devs * sizeof(*devices));

	first_super.md_magic = 0;
	for (i=0; i<num_devs; i++)
		best[i] = -1;

	if (verbose)
	    fprintf(stderr, Name ": looking for devices for %s\n",
		    mddev);

	while ( devlist) {
		char *devname;
		int this_uuid[4];
		int dfd;
		struct stat stb;
		int havesuper=0;

		devname = devlist->devname;
		devlist = devlist->next;

		if (ident->devices &&
		    !match_oneof(ident->devices, devname)) {
			if (inargv || verbose)
				fprintf(stderr, Name ": %s is not one of %s\n", devname, ident->devices);
			continue;
		}
		
		dfd = open(devname, O_RDONLY|O_EXCL, 0);
		if (dfd < 0) {
			if (inargv || verbose)
				fprintf(stderr, Name ": cannot open device %s: %s\n",
					devname, strerror(errno));
		} else if (fstat(dfd, &stb)< 0) {
			/* Impossible! */
			fprintf(stderr, Name ": fstat failed for %s: %s\n",
				devname, strerror(errno));
			close(dfd);
		} else if ((stb.st_mode & S_IFMT) != S_IFBLK) {
			fprintf(stderr, Name ": %s is not a block device.\n",
				devname);
			close(dfd);
		} else if (load_super(dfd, &super)) {
			if (inargv || verbose)
				fprintf( stderr, Name ": no RAID superblock on %s\n",
					 devname);
			close(dfd);
		} else {
			havesuper =1;
			uuid_from_super(this_uuid, &super);
			close(dfd);
		}

		if (ident->uuid_set &&
		    (!havesuper || same_uuid(this_uuid, ident->uuid)==0)) {
			if (inargv || verbose)
				fprintf(stderr, Name ": %s has wrong uuid.\n",
					devname);
			continue;
		}
		if (ident->super_minor != UnSet &&
		    (!havesuper || ident->super_minor != super.md_minor)) {
			if (inargv || verbose)
				fprintf(stderr, Name ": %s has wrong super-minor.\n",
					devname);
			continue;
		}
		if (ident->level != UnSet &&
		    (!havesuper|| ident->level != (int)super.level)) {
			if (inargv || verbose)
				fprintf(stderr, Name ": %s has wrong raid level.\n",
					devname);
			continue;
		}
		if (ident->raid_disks != UnSet &&
		    (!havesuper || ident->raid_disks!= super.raid_disks)) {
			if (inargv || verbose)
				fprintf(stderr, Name ": %s requires wrong number of drives.\n",
					devname);
			continue;
		}

		/* If we are this far, then we are commited to this device.
		 * If the super_block doesn't exist, or doesn't match others,
		 * then we cannot continue
		 */

		if (!havesuper) {
			fprintf(stderr, Name ": %s has no superblock - assembly aborted\n",
				devname);
			return 1;
		}
		if (compare_super(&first_super, &super)) {
			fprintf(stderr, Name ": superblock on %s doesn't match others - assembly aborted\n",
				devname);
			return 1;
		}


		/* this is needed until we get a more relaxed super block format */
		if (devcnt >= MD_SB_DISKS) {
		    fprintf(stderr, Name ": ouch - too many devices appear to be in this array. Ignoring %s\n",
			    devname);
		    continue;
		}
		
		/* looks like a good enough match to update the super block if needed */
		if (update) {
			if (strcmp(update, "sparc2.2")==0 ) {
				/* 2.2 sparc put the events in the wrong place
				 * So we copy the tail of the superblock
				 * up 4 bytes before continuing
				 */
				__u32 *sb32 = (__u32*)&super;
				memcpy(sb32+MD_SB_GENERIC_CONSTANT_WORDS+7,
				       sb32+MD_SB_GENERIC_CONSTANT_WORDS+7+1,
				       (MD_SB_WORDS - (MD_SB_GENERIC_CONSTANT_WORDS+7+1))*4);
				fprintf (stderr, Name ": adjusting superblock of %s for 2.2/sparc compatability.\n",
					 devname);
			}
			if (strcmp(update, "super-minor") ==0) {
				struct stat stb2;
				fstat(mdfd, &stb2);
				super.md_minor = minor(stb2.st_rdev);
				if (verbose)
					fprintf(stderr, Name ": updating superblock of %s with minor number %d\n",
						devname, super.md_minor);
			}
			if (strcmp(update, "summaries") == 0) {
				/* set nr_disks, active_disks, working_disks,
				 * failed_disks, spare_disks based on disks[] 
				 * array in superblock.
				 * Also make sure extra slots aren't 'failed'
				 */
				super.nr_disks = super.active_disks =
					super.working_disks = super.failed_disks =
					super.spare_disks = 0;
				for (i=0; i < MD_SB_DISKS ; i++) 
					if (super.disks[i].major ||
					    super.disks[i].minor) {
						int state = super.disks[i].state;
						if (state & (1<<MD_DISK_REMOVED))
							continue;
						super.nr_disks++;
						if (state & (1<<MD_DISK_ACTIVE))
							super.active_disks++;
						if (state & (1<<MD_DISK_FAULTY))
							super.failed_disks++;
						else
							super.working_disks++;
						if (state == 0)
							super.spare_disks++;
					} else if (i >= super.raid_disks && super.disks[i].number == 0)
						super.disks[i].state = 0;
			}
			if (strcmp(update, "resync") == 0) {
				/* make sure resync happens */
				super.state &= ~(1<<MD_SB_CLEAN);
				super.recovery_cp = 0;
			}
			super.sb_csum = calc_sb_csum(&super);
			dfd = open(devname, O_RDWR|O_EXCL, 0);
			if (dfd < 0) 
				fprintf(stderr, Name ": Cannot open %s for superblock update\n",
					devname);
			else if (store_super(dfd, &super))
				fprintf(stderr, Name ": Could not re-write superblock on %s.\n",
					devname);
			if (dfd >= 0)
				close(dfd);
		}

		if (verbose)
			fprintf(stderr, Name ": %s is identified as a member of %s, slot %d.\n",
				devname, mddev, super.this_disk.raid_disk);
		devices[devcnt].devname = devname;
		devices[devcnt].major = major(stb.st_rdev);
		devices[devcnt].minor = minor(stb.st_rdev);
		devices[devcnt].oldmajor = super.this_disk.major;
		devices[devcnt].oldminor = super.this_disk.minor;
		devices[devcnt].events = md_event(&super);
		devices[devcnt].utime = super.utime;
		devices[devcnt].raid_disk = super.this_disk.raid_disk;
		devices[devcnt].uptodate = 0;
		devices[devcnt].state = super.this_disk.state;
		if (most_recent < devcnt) {
			if (devices[devcnt].events
			    > devices[most_recent].events)
				most_recent = devcnt;
		}
		if ((int)super.level == -4) 
			/* with multipath, the raid_disk from the superblock is meaningless */
			i = devcnt;
		else
			i = devices[devcnt].raid_disk;
		if (i < 10000) {
			if (i >= bestcnt) {
				unsigned int newbestcnt = i+10;
				int *newbest = malloc(sizeof(int)*newbestcnt);
				unsigned int c;
				for (c=0; c < newbestcnt; c++)
					if (c < bestcnt)
						newbest[c] = best[c];
					else
						newbest[c] = -1;
				if (best)free(best);
				best = newbest;
				bestcnt = newbestcnt;
			}
			if (best[i] == -1
			    || devices[best[i]].events < devices[devcnt].events)
				best[i] = devcnt;
		}
		devcnt++;
	}

	if (devcnt == 0) {
		fprintf(stderr, Name ": no devices found for %s\n",
			mddev);
		return 1;
	}
	/* now we have some devices that might be suitable.
	 * I wonder how many
	 */
	avail = malloc(first_super.raid_disks);
	memset(avail, 0, first_super.raid_disks);
	okcnt = 0;
	sparecnt=0;
	for (i=0; i< bestcnt ;i++) {
		int j = best[i];
		int event_margin = !force;
		if (j < 0) continue;
		/* note: we ignore error flags in multipath arrays
		 * as they don't make sense
		 */
		if ((int)first_super.level != -4)
			if (!(devices[j].state & (1<<MD_DISK_SYNC))) {
				if (!(devices[j].state & (1<<MD_DISK_FAULTY)))
					sparecnt++;
				continue;
			}
		if (devices[j].events+event_margin >=
		    devices[most_recent].events) {
			devices[j].uptodate = 1;
			if (i < first_super.raid_disks) {
				okcnt++;
				avail[i]=1;
			} else
				sparecnt++;
		}
	}
	while (force && !enough(first_super.level, first_super.raid_disks,
				first_super.layout,
				avail, okcnt)) {
		/* Choose the newest best drive which is
		 * not up-to-date, update the superblock
		 * and add it.
		 */
		int fd;
		chosen_drive = -1;
		for (i=0; i<first_super.raid_disks && i < bestcnt; i++) {
			int j = best[i];
			if (j>=0 &&
			    !devices[j].uptodate &&
			    devices[j].events > 0 &&
			    (chosen_drive < 0 ||
			     devices[j].events > devices[chosen_drive].events))
				chosen_drive = j;
		}
		if (chosen_drive < 0)
			break;
		fprintf(stderr, Name ": forcing event count in %s(%d) from %d upto %d\n",
			devices[chosen_drive].devname, devices[chosen_drive].raid_disk,
			(int)(devices[chosen_drive].events),
			(int)(devices[most_recent].events));
		fd = open(devices[chosen_drive].devname, O_RDWR|O_EXCL);
		if (fd < 0) {
			fprintf(stderr, Name ": Couldn't open %s for write - not updating\n",
				devices[chosen_drive].devname);
			devices[chosen_drive].events = 0;
			continue;
		}
		if (load_super(fd, &super)) {
			close(fd);
			fprintf(stderr, Name ": RAID superblock disappeared from %s - not updating.\n",
				devices[chosen_drive].devname);
			devices[chosen_drive].events = 0;
			continue;
		}
		super.events_hi = (devices[most_recent].events>>32)&0xFFFFFFFF;
		super.events_lo = (devices[most_recent].events)&0xFFFFFFFF;
		if (super.level == 5 || super.level == 4) {
			/* need to force clean */
			super.state = (1<<MD_SB_CLEAN);
		}
		super.sb_csum = calc_sb_csum(&super);
/*DRYRUN*/	if (store_super(fd, &super)) {
			close(fd);
			fprintf(stderr, Name ": Could not re-write superblock on %s\n",
				devices[chosen_drive].devname);
			devices[chosen_drive].events = 0;
			continue;
		}
		close(fd);
		devices[chosen_drive].events = devices[most_recent].events;
		devices[chosen_drive].uptodate = 1;
		avail[chosen_drive] = 1;
		okcnt++;
	}

	/* Now we want to look at the superblock which the kernel will base things on
	 * and compare the devices that we think are working with the devices that the
	 * superblock thinks are working.
	 * If there are differences and --force is given, then update this chosen
	 * superblock.
	 */
	chosen_drive = -1;
	for (i=0; chosen_drive < 0 && i<bestcnt; i++) {
		int j = best[i];
		int fd;
		if (j<0)
			continue;
		if (!devices[j].uptodate)
			continue;
		chosen_drive = j;
		if ((fd=open(devices[j].devname, O_RDONLY|O_EXCL))< 0) {
			fprintf(stderr, Name ": Cannot open %s: %s\n",
				devices[j].devname, strerror(errno));
			return 1;
		}
		if (load_super(fd, &super)) {
			close(fd);
			fprintf(stderr, Name ": RAID superblock has disappeared from %s\n",
				devices[j].devname);
			return 1;
		}
		close(fd);
	}

	for (i=0; i<bestcnt; i++) {
		int j = best[i];
		unsigned int desired_state;

		if (i < super.raid_disks)
			desired_state = (1<<MD_DISK_ACTIVE) | (1<<MD_DISK_SYNC);
		else
			desired_state = 0;

		if (j<0)
			continue;
		if (!devices[j].uptodate)
			continue;
#if 0
This doesnt work yet 
		if (devices[j].major != super.disks[i].major ||
		    devices[j].minor != super.disks[i].minor) {
			change |= 1;
			super.disks[i].major = devices[j].major;
			super.disks[i].minor = devices[j].minor;
		}
#endif 
		if (devices[j].oldmajor != super.disks[i].major ||
		    devices[j].oldminor != super.disks[i].minor) {
			change |= 2;
			super.disks[i].major = devices[j].oldmajor;
			super.disks[i].minor = devices[j].oldminor;
		}
		if (devices[j].uptodate &&
		    (super.disks[i].state != desired_state)) {
			if (force) {
				fprintf(stderr, Name ": "
					"clearing FAULTY flag for device %d in %s for %s\n",
					j, mddev, devices[j].devname);
				super.disks[i].state = desired_state;
				change |= 2;
			} else {
				fprintf(stderr, Name ": "
					"device %d in %s has wrong state in superblock, but %s seems ok\n",
					i, mddev, devices[j].devname);
			}
		}
		if (!devices[j].uptodate &&
		    !(super.disks[i].state & (1 << MD_DISK_FAULTY))) {
			fprintf(stderr, Name ": devices %d of %s is not marked FAULTY in superblock, but cannot be found\n",
				i, mddev);
		}
	}
	if (force && (super.level == 4 || super.level == 5) && 
	    okcnt == super.raid_disks-1) {
		super.state = (1<< MD_SB_CLEAN);
		change |= 2;
	}

	if ((force && (change & 2))
	    || (old_linux && (change & 1))) {
		int fd;
		super.sb_csum = calc_sb_csum(&super);
		fd = open(devices[chosen_drive].devname, O_RDWR|O_EXCL);
		if (fd < 0) {
			fprintf(stderr, Name ": Could open %s for write - cannot Assemble array.\n",
				devices[chosen_drive].devname);
			return 1;
		}
		if (store_super(fd, &super)) {
			close(fd);
			fprintf(stderr, Name ": Could not re-write superblock on %s\n",
				devices[chosen_drive].devname);
			return 1;
		}
		close(fd);
		change = 0;
	}

	/* count number of in-sync devices according to the superblock.
	 * We must have this number to start the array without -s or -R
	 */
	req_cnt = 0;
	for (i=0; i<MD_SB_DISKS; i++)
		if ((first_super.disks[i].state & (1<<MD_DISK_SYNC)) &&
		    (first_super.disks[i].state & (1<<MD_DISK_ACTIVE)) &&
		    !(first_super.disks[i].state & (1<<MD_DISK_FAULTY)))
			req_cnt ++;
									    

	/* Almost ready to actually *do* something */
	if (!old_linux) {
		if (ioctl(mdfd, SET_ARRAY_INFO, NULL) != 0) {
			fprintf(stderr, Name ": SET_ARRAY_INFO failed for %s: %s\n",
				mddev, strerror(errno));
			return 1;
		}
		/* First, add the raid disks, but add the chosen one last */
		for (i=0; i<= bestcnt; i++) {
			int j;
			if (i < bestcnt) {
				j = best[i];
				if (j == chosen_drive)
					continue;
			} else
				j = chosen_drive;

			if (j >= 0 /* && devices[j].uptodate */) {
				mdu_disk_info_t disk;
				memset(&disk, 0, sizeof(disk));
				disk.major = devices[j].major;
				disk.minor = devices[j].minor;
				if (ioctl(mdfd, ADD_NEW_DISK, &disk)!=0) {
					fprintf(stderr, Name ": failed to add %s to %s: %s\n",
						devices[j].devname,
						mddev,
						strerror(errno));
					if (i < first_super.raid_disks || i == bestcnt)
						okcnt--;
					else
						sparecnt--;
				} else if (verbose)
					fprintf(stderr, Name ": added %s to %s as %d\n",
						devices[j].devname, mddev, devices[j].raid_disk);
			} else if (verbose && i < first_super.raid_disks)
				fprintf(stderr, Name ": no uptodate device for slot %d of %s\n",
					i, mddev);
		}
		
		if (runstop == 1 ||
		    (runstop == 0 && 
		     ( enough(first_super.level, first_super.raid_disks, first_super.layout, avail, okcnt) &&
		       (okcnt >= req_cnt || start_partial_ok)
			     ))) {
			if (ioctl(mdfd, RUN_ARRAY, NULL)==0) {
				fprintf(stderr, Name ": %s has been started with %d drive%s",
					mddev, okcnt, okcnt==1?"":"s");
				if (okcnt < first_super.raid_disks) 
					fprintf(stderr, " (out of %d)", first_super.raid_disks);
				if (sparecnt)
					fprintf(stderr, " and %d spare%s", sparecnt, sparecnt==1?"":"s");
				fprintf(stderr, ".\n");
				return 0;
			}
			fprintf(stderr, Name ": failed to RUN_ARRAY %s: %s\n",
				mddev, strerror(errno));
			return 1;
		}
		if (runstop == -1) {
			fprintf(stderr, Name ": %s assembled from %d drive%s, but not started.\n",
				mddev, okcnt, okcnt==1?"":"s");
			return 0;
		}
		fprintf(stderr, Name ": %s assembled from %d drive%s", mddev, okcnt, okcnt==1?"":"s");
		if (sparecnt)
			fprintf(stderr, " and %d spare%s", sparecnt, sparecnt==1?"":"s");
		if (!enough(first_super.level, first_super.raid_disks, first_super.layout, avail, okcnt))
			fprintf(stderr, " - not enough to start the array.\n");
		else {
			if (req_cnt == first_super.raid_disks)
				fprintf(stderr, " - need all %d to start it", req_cnt);
			else
				fprintf(stderr, " - need %d of %d to start", req_cnt, first_super.raid_disks);
			fprintf(stderr, " (use --run to insist).\n");
		}
		return 1;
	} else {
		/* The "chosen_drive" is a good choice, and if necessary, the superblock has
		 * been updated to point to the current locations of devices.
		 * so we can just start the array
		 */
		unsigned long dev;
		dev = makedev(devices[chosen_drive].major,
			    devices[chosen_drive].minor);
		if (ioctl(mdfd, START_ARRAY, dev)) {
		    fprintf(stderr, Name ": Cannot start array: %s\n",
			    strerror(errno));
		}
		
	}
	return 0;
}
