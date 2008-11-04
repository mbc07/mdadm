/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2006 Neil Brown <neilb@suse.de>
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

#include "mdadm.h"
#include "md_p.h"
#include <ctype.h>


void make_parts(char *dev, int cnt)
{
	/* make 'cnt' partition devices for 'dev'
	 * If dev is a device name we use the
	 *  major/minor from dev and add 1..cnt
	 * If it is a symlink, we make similar symlinks.
	 * If dev ends with a digit, we add "p%d" else "%d"
	 * If the name exists, we use it's owner/mode,
	 * else that of dev
	 */
	struct stat stb;
	int major_num = major_num; /* quiet gcc -Os unitialized warning */
	int minor_num = minor_num; /* quiet gcc -Os unitialized warning */
	int odig = odig; /* quiet gcc -Os unitialized warning */
	int i;
	int nlen = strlen(dev) + 20;
	char *name = malloc(nlen);
	int dig = isdigit(dev[strlen(dev)-1]);
	char orig[1024];
	char sym[1024];

	if (cnt==0) cnt=4;
	if (lstat(dev, &stb)!= 0)
		return;
	if (S_ISLNK(stb.st_mode)) {
		int len = readlink(dev, orig, sizeof(orig));
		if (len < 0 || len > 1000)
			return;
		orig[len] = 0;
		odig = isdigit(orig[len-1]);
	} else if (S_ISBLK(stb.st_mode)) {
		major_num = major(stb.st_rdev);
		minor_num = minor(stb.st_rdev);
	} else
		   return;
	for (i=1; i <= cnt ; i++) {
		struct stat stb2;
		snprintf(name, nlen, "%s%s%d", dev, dig?"p":"", i);
		if (stat(name, &stb2)==0) {
			if (!S_ISBLK(stb2.st_mode))
				continue;
			if (stb2.st_rdev == makedev(major_num, minor_num+i))
				continue;
			unlink(name);
		} else {
			stb2 = stb;
		}
		if (S_ISBLK(stb.st_mode)) {
			if (mknod(name, S_IFBLK | 0600,
				  makedev(major_num, minor_num+i)))
				perror("mknod");
			if (chown(name, stb2.st_uid, stb2.st_gid))
				perror("chown");
			if (chmod(name, stb2.st_mode & 07777))
				perror("chmod");
		} else {
			snprintf(sym, 10000, "%s%s%d", orig, odig?"p":"", i);
			symlink(sym, name);
		}
		stat(name, &stb2);
		add_dev(name, &stb2, 0, NULL);
	}
}


/*
 * We need a new md device to assemble/build/create an array.
 * 'dev' is a name given us by the user (command line or mdadm.conf)
 * It might start with /dev or /dev/md any might end with a digit
 * string.
 * If it starts with just /dev, it must be /dev/mdX or /dev/md_dX
 * If it ends with a digit string, then it must be as above, or
 * 'trustworthy' must be 'METADATA' and the 'dev' must be
 *  /dev/md/'name'NN or 'name'NN
 * If it doesn't end with a digit string, it must be /dev/md/'name'
 * or 'name' or must be NULL.
 * If the digit string is present, it gives the minor number to use
 * If not, we choose a high, unused minor number.
 * If the 'dev' is a standard name, it devices whether 'md' or 'mdp'.
 * else if the name is 'd[0-9]+' then we use mdp
 * else if trustworthy is 'METADATA' we use md
 * else the choice depends on 'autof'.
 * If name is NULL it is assumed to match whatever dev provides.
 * If both name and dev are NULL, we choose a name 'mdXX' or 'mdpXX'
 *
 * If 'name' is given, and 'trustworthy' is 'foreign' and name is not
 * supported by 'dev', we add a "_%d" suffix based on the minor number
 * use that.
 *
 * If udev is configured, we create a temporary device, open it, and 
 * unlink it.
 * If not, we create the /dev/mdXX device, and is name is usable,
 * /dev/md/name
 * In any case we return /dev/md/name or (if that isn't available)
 * /dev/mdXX in 'chosen'.
 *
 * When we create devices, we use uid/gid/umask from config file.
 */

int create_mddev(char *dev, char *name, int autof, int trustworthy,
		 char *chosen)
{
	int mdfd;
	struct stat stb;
	int num = -1;
	int use_mdp = -1;
	struct createinfo *ci = conf_get_create_info();
	int parts;
	char *cname;
	char devname[20];
	char cbuf[400];
	if (chosen == NULL)
		chosen = cbuf;


	if (autof == 0)
		autof = ci->autof;

	parts = autof >> 3;
	autof &= 7;

	strcpy(chosen, "/dev/md/");
	cname = chosen + strlen(chosen);


	if (dev) {
		
		if (strncmp(dev, "/dev/md/", 8) == 0) {
			strcpy(cname, dev+8);
		} else if (strncmp(dev, "/dev/", 5) == 0) {
			char *e = dev + strlen(dev);
			while (e > dev && isdigit(e[-1]))
				e--;
			if (e[0])
				num = strtoul(e, NULL, 10);
			strcpy(cname, dev+5);
			cname[e-(dev+5)] = 0;
			/* name *must* be mdXX or md_dXX in this context */
			if (num < 0 ||
			    (strcmp(cname, "md") != 0 && strcmp(cname, "md_d") != 0)) {
				fprintf(stderr, Name ": %s is an invalid name "
					"for an md device.  Try /dev/md/%s\n",
					dev, dev+5);
				return -1;
			}
			if (strcmp(cname, "md") == 0)
				use_mdp = 0;
			else
				use_mdp = 1;
			/* recreate name: /dev/md/0 or /dev/md/d0 */
			sprintf(cname, "%s%d", use_mdp?"d":"", num);
		} else
			strcpy(cname, dev);

		/* 'cname' must not contain a slash, and may not be
		 * empty.
		 */
		if (strchr(cname, '/') != NULL) {
			fprintf(stderr, Name ": %s is an invalid name "
				"for an md device.\n", dev);
			return -1;
		}
		if (cname[0] == 0) {
			fprintf(stderr, Name ": %s is an invalid name "
				"for an md device (empty!).", dev);
			return -1;
		}
		if (num < 0) {
			/* If cname  is 'N' or 'dN', we get dev number
			 * from there.
			 */
			char *sp = cname;
			char *ep;
			if (cname[0] == 'd')
				sp++;
			num = strtoul(sp, &ep, 10);
			if (ep == sp || *ep || num < 0)
				num = -1;
			else if (cname[0] == 'd')
				use_mdp = 1;
			else
				use_mdp = 0;
		}
	}

	/* Now determine device number */
	/* named 'METADATA' cannot use 'mdp'. */
	if (name && name[0] == 0)
		name = NULL;
	if (name && trustworthy == METADATA && use_mdp == 1) {
		fprintf(stderr, Name ": %s is not allowed for a %s container. "
			"Consider /dev/md%d.\n", dev, name, num);
		return -1;
	}
	if (name && trustworthy == METADATA)
		use_mdp = 0;
	if (use_mdp == -1) {
		if (autof == 4 || autof == 6)
			use_mdp = 1;
		else
			use_mdp = 0;
	}
	if (num < 0 && trustworthy == LOCAL && name) {
		/* if name is numeric, us that for num */
		char *ep;
		num = strtoul(name, &ep, 10);
		if (ep == name || *ep)
			num = -1;
	}

	if (num < 0) {
		/* need to choose a free number. */
		num = find_free_devnum(use_mdp);
		if (num == NoMdDev) {
			fprintf(stderr, Name ": No avail md devices - aborting\n");
			return -1;
		}
	} else {
		num = use_mdp ? (-1-num) : num;
		if (mddev_busy(num)) {
			fprintf(stderr, Name ": %s is already in use.\n",
				dev);
			return -1;
		}
	}

	if (num < 0)
		sprintf(devname, "/dev/md_d%d", -1-num);
	else
		sprintf(devname, "/dev/md%d", num);

	if (cname[0] == 0 && name) {
		/* Need to find a name if we can
		 * We don't completely trust 'name'.  Truncate to
		 * reasonable length and remove '/'
		 */
		char *cp;
		strncpy(cname, name, 200);
		cname[200] = 0;
		while ((cp = strchr(cname, '/')) != NULL)
			*cp = '-';
		if (trustworthy == METADATA)
			/* always add device number to metadata */
			sprintf(cname+strlen(cname), "%d", num);
		else if (trustworthy == FOREIGN &&
			 strchr(cname, ':') == NULL)
			/* add _%d to FOREIGN array that don't have
			 * a 'host:' prefix
			 */
			sprintf(cname+strlen(cname), "_%d", num<0?(-1-num):num);
	}
	if (cname[0] == 0)
		strcpy(chosen, devname);

	/* We have a device number and name.
	 * If we cannot detect udev, we need to make
	 * devices and links ourselves.
	 */
	if (stat("/dev/.udev", &stb) != 0 ||
	    check_env("MDADM_NO_UDEV")) {
		/* Make sure 'devname' exists and 'chosen' is a symlink to it */
		if (lstat(devname, &stb) == 0) {
			/* Must be the correct device, else error */
			if ((stb.st_mode&S_IFMT) != S_IFBLK ||
			    stb.st_rdev != makedev(dev2major(num),dev2minor(num))) {
				fprintf(stderr, Name ": %s exists but looks wrong, please fix\n",
					devname);
				return -1;
			}
		} else {
			if (mknod(devname, S_IFBLK|0600,
				  makedev(dev2major(num),dev2minor(num))) != 0) {
				fprintf(stderr, Name ": failed to create %s\n",
					devname);
				return -1;
			}
			if (chown(devname, ci->uid, ci->gid))
				perror("chown");
			if (chmod(devname, ci->mode))
				perror("chmod");
			stat(devname, &stb);
			add_dev(devname, &stb, 0, NULL);
		}
		if (use_mdp == 1)
			make_parts(devname, parts);
		if (strcmp(chosen, devname) != 0) {

			if (mkdir("/dev/md",0700)==0) {
				if (chown("/dev/md", ci->uid, ci->gid))
					perror("chown /dev/md");
				if (chmod("/dev/md", ci->mode| ((ci->mode>>2) & 0111)))
					perror("chmod /dev/md");
			}

			if (dev && strcmp(chosen, dev) == 0)
				/* We know we are allowed to use this name */
				unlink(chosen);

			if (lstat(chosen, &stb) == 0) {
				char buf[300];
				if ((stb.st_mode & S_IFMT) != S_IFLNK ||
				    readlink(chosen, buf, 300) <0 ||
				    strcmp(buf, devname) != 0) {
					fprintf(stderr, Name ": %s exists - ignoring\n",
						chosen);
					strcpy(chosen, devname);
				}
			} else
				symlink(devname, chosen);
			if (use_mdp && strcmp(chosen, devname) != 0)
				make_parts(chosen, parts);
		}
	}
	mdfd = open_dev_excl(num);
	if (mdfd < 0)
		fprintf(stderr, Name ": unexpected failure opening %s\n",
			devname);
	return mdfd;
}


/* Open this and check that it is an md device.
 * On success, return filedescriptor.
 * On failure, return -1 if it doesn't exist,
 * or -2 if it exists but is not an md device.
 */
int open_mddev(char *dev, int report_errors)
{
	int mdfd = open(dev, O_RDWR);
	if (mdfd < 0) {
		if (report_errors)
			fprintf(stderr, Name ": error opening %s: %s\n",
				dev, strerror(errno));
		return -1;
	}
	if (md_get_version(mdfd) <= 0) {
		close(mdfd);
		if (report_errors)
			fprintf(stderr, Name ": %s does not appear to be "
				"an md device\n", dev);
		return -2;
	}
	return mdfd;
}
