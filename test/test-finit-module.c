/*
 * Copyright (C) 2023 Luis Chamberlain <mcgrof@kernel.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include <linux/module.h>

int main(void)
{
	char *module = "hello";
	int fd, ret;

	fd = open(module, O_RDONLY | O_CLOEXEC);
	if (fd > 0)
		ret = finit_module(fd, finit_args1, kernel_flags);

	return 0;
}
